#include "vrf/ui/profiler_panel.hpp"

#include <algorithm>

#include "vrf/gpu/gpu_profiler.hpp"

#if defined(VRF_WITH_IMGUI)
#include <imgui.h>
#endif

namespace vrf::ui
{
    ProfilerPanel::Entry& ProfilerPanel::FindOrAdd(const std::string& name, uint32_t depth)
    {
        // Linear scan: a frame's zone count is tens, and keeping insertion order means the tree
        // renders in record order, which is the order the passes actually ran. A map would sort
        // by name and lose that.
        for (Entry& entry : m_entries)
        {
            if (entry.depth == depth && entry.name == name)
                return entry;
        }
        Entry fresh;
        fresh.name  = name;
        fresh.depth = depth;
        m_entries.push_back(std::move(fresh));
        return m_entries.back();
    }

    void ProfilerPanel::Update(const GpuProfiler& profiler)
    {
        if (paused || !profiler.Enabled())
            return;

        const auto& zones = profiler.Results();

        for (Entry& entry : m_entries)
            entry.occurrences = 0;

        for (const auto& zone : zones)
        {
            Entry& entry = FindOrAdd(zone.name, zone.depth);
            if (entry.occurrences == 0)
                entry.instantMs = zone.milliseconds;
            else
                entry.instantMs += zone.milliseconds; // same name twice in a frame: sum them
            ++entry.occurrences;
        }

        const double alpha = std::clamp(options.smoothing, 0.0, 1.0);
        for (Entry& entry : m_entries)
        {
            // A zone absent this frame decays toward zero rather than freezing at its last
            // value, otherwise a pass that stopped running keeps advertising a cost.
            const double sample = entry.occurrences > 0 ? entry.instantMs : 0.0;
            if (entry.occurrences == 0)
                entry.instantMs = 0.0;
            entry.averageMs = entry.averageMs + alpha * (sample - entry.averageMs);

            if (sample >= entry.peakMs || entry.peakAgeFrames >= options.peakHoldFrames)
            {
                entry.peakMs        = sample;
                entry.peakAgeFrames = 0;
            }
            else
            {
                ++entry.peakAgeFrames;
            }
        }

        m_totalInstantMs = profiler.LastFrameMs();
        m_totalAverageMs = m_totalAverageMs + alpha * (m_totalInstantMs - m_totalAverageMs);

        // Just report the count. Zones past GpuProfiler's maxZonesPerFrame are dropped and a
        // dropped zone reads as free, so this is worth surfacing - but it cannot be *detected*
        // from the results alone: the profiler does not expose its cap, and a genuinely fast
        // pass can legitimately resolve to 0.000 ms. Inferring "records dropped" from a
        // zero-length zone would fire on healthy frames and send someone chasing nothing, so
        // the panel shows the number and names the cap in a tooltip instead of guessing.
        m_observedZones = static_cast<uint32_t>(zones.size());
        m_peakZones     = std::max(m_peakZones, m_observedZones);
    }

    void ProfilerPanel::Reset() noexcept
    {
        m_entries.clear();
        m_totalInstantMs = 0.0;
        m_totalAverageMs = 0.0;
        m_observedZones  = 0;
        m_peakZones      = 0;
    }

#if defined(VRF_WITH_IMGUI)
    void ProfilerPanel::Draw()
    {
        ImGui::Checkbox("Pause", &paused);
        ImGui::SameLine();
        if (ImGui::Button("Reset"))
            Reset();
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("GPU %.3f ms (avg)", m_totalAverageMs);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Sum of the outermost zones. Instant %.3f ms.\n"
                              "Read the average - single-frame timestamp deltas jitter more\n"
                              "than most differences you are looking for.",
                              m_totalInstantMs);
        }

        ImGui::SameLine();
        ImGui::TextDisabled("| zones %u (peak %u)", m_observedZones, m_peakZones);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("GpuProfiler drops zones past maxZonesPerFrame (default 64), and a\n"
                              "dropped zone reads as free rather than as an error. If this count\n"
                              "sits at your cap, raise it to at least the pass count.");
        }

        if (m_entries.empty())
        {
            ImGui::TextDisabled("no zones recorded (is the profiler enabled and are zones opened?)");
            return;
        }

        int columns = 2;
        if (options.showInstantColumn)
            ++columns;
        if (options.showPeakColumn)
            ++columns;
        if (options.showPercentColumn)
            ++columns;

        constexpr ImGuiTableFlags kFlags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##vrfProfilerZones", columns, kFlags))
            return;

        ImGui::TableSetupColumn("zone", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("avg ms");
        if (options.showInstantColumn)
            ImGui::TableSetupColumn("now");
        if (options.showPeakColumn)
            ImGui::TableSetupColumn("peak");
        if (options.showPercentColumn)
            ImGui::TableSetupColumn("%");
        ImGui::TableHeadersRow();

        for (const Entry& entry : m_entries)
        {
            if (entry.averageMs < options.minVisibleMs && entry.occurrences == 0)
                continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // Depth as indentation rather than a collapsible tree: these rows are read as a
            // profile, and a collapsed parent would hide exactly the child that regressed.
            if (entry.depth > 0)
                ImGui::Indent(static_cast<float>(entry.depth) * 12.0f);
            if (entry.occurrences > 1)
                ImGui::Text("%s (x%u)", entry.name.c_str(), entry.occurrences);
            else if (entry.occurrences == 0)
                ImGui::TextDisabled("%s", entry.name.c_str());
            else
                ImGui::TextUnformatted(entry.name.c_str());
            if (entry.depth > 0)
                ImGui::Unindent(static_cast<float>(entry.depth) * 12.0f);

            ImGui::TableNextColumn();
            ImGui::Text("%.3f", entry.averageMs);
            if (options.showInstantColumn)
            {
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%.3f", entry.instantMs);
            }
            if (options.showPeakColumn)
            {
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%.3f", entry.peakMs);
            }
            if (options.showPercentColumn)
            {
                ImGui::TableNextColumn();
                const double share = m_totalAverageMs > 0.0 ? 100.0 * entry.averageMs / m_totalAverageMs : 0.0;
                ImGui::TextDisabled("%5.1f", share);
            }
        }

        ImGui::EndTable();
    }

    void ProfilerPanel::DrawWindow(const char* title, bool* open)
    {
        if (open != nullptr && !*open)
            return;
        if (ImGui::Begin(title, open))
            Draw();
        ImGui::End();
    }
#else
    void ProfilerPanel::Draw() {}
    void ProfilerPanel::DrawWindow(const char*, bool*) {}
#endif
} // namespace vrf::ui
