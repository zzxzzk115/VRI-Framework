/*
 * profiler_panel.hpp - an ImGui view of GpuProfiler results (and optional CPU zones).
 *
 * Renders the nested zones GpuProfiler resolved last frame as a tree, with each zone's share
 * of the frame. Requires vrf_with_imgui; without it the class still compiles and its Draw calls
 * do nothing, so callers never need to branch.
 *
 * WHY THIS SMOOTHS BY DEFAULT. Raw timestamp deltas jitter frame to frame by more than the
 * differences you usually care about, so an unsmoothed readout is not just noisy, it is
 * misleading - the eye latches onto whichever number happens to spike. The panel keeps an
 * exponential moving average per zone and shows that, with the instantaneous and peak values
 * alongside. Read `avg`; use `max` to catch hitches.
 *
 * Zone identity is the zone *name*, so names must be stable across frames for the average to
 * mean anything. Two zones sharing a name at different tree depths are aggregated together,
 * which is usually what you want for a pass that runs more than once per frame.
 *
 *   ProfilerPanel panel;
 *   ...
 *   profiler.EndFrame(cmd);
 *   panel.Update(profiler);                       // once per frame, after results resolve
 *   panel.DrawWindow("GPU Profiler", &open);      // or Draw() inside your own window
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrf
{
    class GpuProfiler;
}

namespace vrf::ui
{
    class ProfilerPanel
    {
    public:
        struct Options
        {
            // Weight of the newest frame in the moving average. 1.0 disables smoothing.
            double smoothing {0.05};
            // Frames a peak is held before it decays back toward the average, so a hitch stays
            // readable instead of vanishing before it can be seen.
            uint32_t peakHoldFrames {90};
            // Hide zones below this many milliseconds. A frame graph has a long tail of
            // sub-microsecond passes that only add scroll.
            double minVisibleMs {0.0};
            bool   showPercentColumn {true};
            bool   showInstantColumn {true};
            bool   showPeakColumn {true};
        };

        struct Entry
        {
            std::string name;
            uint32_t    depth {0};
            double      instantMs {0.0};
            double      averageMs {0.0};
            double      peakMs {0.0};
            uint32_t    peakAgeFrames {0};
            // Times this name appeared in the frame; >1 means the average aggregates them.
            uint32_t occurrences {0};
        };

        ProfilerPanel() = default;
        explicit ProfilerPanel(const Options& initial) : options {initial} {}

        // Public fields rather than accessors: an accessor named `options` would collide with
        // the nested Options type, and the panel's own checkbox writes `paused` directly.
        Options options {};
        bool    paused {false};

        // Fold this frame's resolved zones into the averages. Call once per frame. Ignored while
        // paused, so the displayed numbers hold still for reading.
        void Update(const GpuProfiler&);

        // Draw the panel body only (no Begin/End), so it composes into an existing window.
        void Draw();
        // Draw wrapped in its own ImGui window. `open` may be null.
        void DrawWindow(const char* title, bool* open);

        void Reset() noexcept;

        [[nodiscard]] double                    TotalMs() const noexcept { return m_totalAverageMs; }
        [[nodiscard]] const std::vector<Entry>& Entries() const noexcept { return m_entries; }

        // Zones resolved in the last frame, and the high-water mark. Compare against the
        // maxZonesPerFrame you passed to GpuProfiler::Create: records past the cap are dropped
        // and a dropped zone reads as free. This is reported rather than detected, because the
        // cap is not queryable and a genuinely fast pass can resolve to 0.000 ms - so a warning
        // inferred from the results would fire on healthy frames.
        [[nodiscard]] uint32_t ObservedZones() const noexcept { return m_observedZones; }
        [[nodiscard]] uint32_t PeakZones() const noexcept { return m_peakZones; }

    private:
        Entry& FindOrAdd(const std::string& name, uint32_t depth);

        std::vector<Entry> m_entries;
        double             m_totalInstantMs {0.0};
        double             m_totalAverageMs {0.0};
        uint32_t           m_observedZones {0};
        uint32_t           m_peakZones {0};
    };
} // namespace vrf::ui
