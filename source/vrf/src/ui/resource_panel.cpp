#include "vrf/ui/resource_panel.hpp"

#if defined(VRF_WITH_IMGUI)

#include <algorithm>
#include <cstdio>
#include <map>
#include <utility>

#include <imgui.h>

#include "vrf/gpu/render_device.hpp"

namespace vrf::ui
{
    namespace
    {
        [[nodiscard]] const char* TypeName(const VriObjectType type)
        {
            switch (type)
            {
                case VriObjectType_Buffer:
                    return "Buffer";
                case VriObjectType_Texture:
                    return "Texture";
                case VriObjectType_AccelerationStructure:
                    return "Accel";
                case VriObjectType_Micromap:
                    return "Micromap";
                default:
                    return "Other";
            }
        }

        [[nodiscard]] std::string FormatBytes(const uint64_t bytes)
        {
            char       buffer[32];
            const auto mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
            if (mb >= 1024.0)
                std::snprintf(buffer, sizeof buffer, "%.2f GB", mb / 1024.0);
            else if (mb >= 1.0)
                std::snprintf(buffer, sizeof buffer, "%.1f MB", mb);
            else
                std::snprintf(buffer, sizeof buffer, "%.0f KB", static_cast<double>(bytes) / 1024.0);
            return buffer;
        }
    } // namespace

    void ResourcePanel::Update(RenderDevice& device, const float deltaSeconds)
    {
        m_since += deltaSeconds;
        if (m_since < m_interval)
            return;
        m_since = 0.0f;

        const VriCoreInterface& core = device.Core();
        if (core.EnumerateObjects == nullptr)
        {
            m_supported = false;
            return;
        }

        uint32_t count = 0;
        if (core.EnumerateObjects(device.Handle(), &count, nullptr) != VriResult_Success)
        {
            m_supported = false;
            return;
        }
        m_supported = true;

        std::vector<VriObjectInfo> objects(count);
        if (count != 0 && core.EnumerateObjects(device.Handle(), &count, objects.data()) != VriResult_Success)
            return;
        objects.resize(count);

        // Grouped by (type, name) rather than listed raw: a framegraph makes dozens of identically
        // named targets and a per-object list buries the total under them. An unnamed object keeps
        // its own row only in the sense that all unnamed objects of a type share one.
        std::map<std::pair<int, std::string>, Row> groups;
        m_totalBytes   = 0;
        m_totalObjects = count;
        for (auto& slot : m_byType)
            slot = 0;

        for (const VriObjectInfo& info : objects)
        {
            const std::string name = info.name[0] != '\0' ? info.name : "<unnamed>";
            auto&             row  = groups[{static_cast<int>(info.type), name}];
            if (row.count == 1 && row.bytes == 0 && row.name.empty())
            {
                row.name     = name;
                row.type     = info.type;
                row.width    = info.width;
                row.height   = info.height;
                row.mipNum   = info.mipNum;
                row.layerNum = info.layerNum;
                row.format   = info.format;
                row.count    = 0;
            }
            row.bytes += info.memoryBytes;
            ++row.count;
            m_totalBytes += info.memoryBytes;
            const auto index = static_cast<size_t>(info.type);
            if (index < std::size(m_byType))
                m_byType[index] += info.memoryBytes;
        }

        m_rows.clear();
        m_rows.reserve(groups.size());
        for (auto& [key, row] : groups)
        {
            Row entry = row;
            if (entry.count != 1)
                entry.width = entry.height = entry.mipNum = entry.layerNum = 0;
            m_rows.push_back(std::move(entry));
        }
        std::sort(m_rows.begin(), m_rows.end(), [](const Row& a, const Row& b) { return a.bytes > b.bytes; });
    }

    void ResourcePanel::Draw()
    {
        if (!m_supported)
        {
            ImGui::TextDisabled("this backend does not track objects");
            return;
        }

        ImGui::Text("%u objects, %s tracked", m_totalObjects, FormatBytes(m_totalBytes).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(what the device owns; the driver's own total is above)");

        for (size_t i = 0; i < std::size(m_byType); ++i)
        {
            if (m_byType[i] == 0)
                continue;
            ImGui::SameLine(0.0f, 16.0f);
            ImGui::TextDisabled("%s %s", TypeName(static_cast<VriObjectType>(i)), FormatBytes(m_byType[i]).c_str());
        }
        ImGui::Separator();

        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                           ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("resources", 5, kFlags))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();

        for (const Row& row : m_rows)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.name.c_str());
            if (row.count == 1 && row.width != 0 && ImGui::IsItemHovered())
            {
                if (row.type == VriObjectType_Texture)
                    ImGui::SetTooltip("%ux%u, %u mips, %u layers, format %d",
                                      row.width,
                                      row.height,
                                      row.mipNum,
                                      row.layerNum,
                                      static_cast<int>(row.format));
                else if (row.type == VriObjectType_AccelerationStructure)
                    ImGui::SetTooltip("store %.1f MB, scratch %.1f MB\n"
                                      "scratch is one-shot: release it after the build's fence",
                                      static_cast<double>(row.width) / 1024.0,
                                      static_cast<double>(row.height) / 1024.0);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(TypeName(row.type));
            ImGui::TableNextColumn();
            ImGui::Text("%u", row.count);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatBytes(row.bytes).c_str());
            ImGui::TableNextColumn();
            const float share =
                m_totalBytes != 0 ?
                    static_cast<float>(static_cast<double>(row.bytes) / static_cast<double>(m_totalBytes)) :
                    0.0f;
            char label[16];
            std::snprintf(label, sizeof label, "%.1f%%", share * 100.0f);
            ImGui::ProgressBar(share, ImVec2 {-1.0f, 0.0f}, label);
        }
        ImGui::EndTable();
    }
} // namespace vrf::ui

#endif // VRF_WITH_IMGUI
