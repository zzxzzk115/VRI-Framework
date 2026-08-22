/*
 * resource_panel.hpp - what the device owns, and what each object cost.
 *
 * The driver's budget query says how much VRAM is in use. It never says what is using it, and on a
 * card where the working set is near the budget that is the only question worth asking: going over
 * makes the driver demote device-local memory to host, and the passes that read the demoted
 * allocation collapse while everything else looks fine. A per-zone timing table sends you looking
 * in the pipeline for something that is not there.
 *
 * Reads VriCoreInterface::EnumerateObjects, so it shows the same thing on every backend. Names come
 * from SetDebugName; unnamed objects group under their type.
 */
#pragma once

#if defined(VRF_WITH_IMGUI)

#include <cstdint>
#include <string>
#include <vector>

#include <vri/vri.h>

namespace vrf
{
    class RenderDevice;
}

namespace vrf::ui
{
    class ResourcePanel
    {
    public:
        // Re-reads the device's object list. Snapshotting takes the device's lock and copies, so
        // this is not free - the panel refreshes on an interval rather than every frame.
        void Update(RenderDevice& device, float deltaSeconds);

        // Draws inside the caller's window; no Begin/End of its own, like ProfilerPanel.
        void Draw();

        // Seconds between snapshots. 0 refreshes every frame.
        void SetRefreshInterval(const float seconds) noexcept { m_interval = seconds; }

    private:
        struct Row
        {
            std::string   name; // SetDebugName's label, or "<unnamed>"
            VriObjectType type {VriObjectType_Unknown};
            uint64_t      bytes {0};
            uint32_t      count {1};
            // Populated only when a group holds exactly one object, so a single texture can show
            // its dimensions without a group of forty pretending to share them.
            uint32_t  width {0}, height {0}, mipNum {0}, layerNum {0};
            VriFormat format {VriFormat_Unknown};
        };

        std::vector<Row> m_rows; // grouped by (type, name), largest first
        uint64_t         m_totalBytes {0};
        uint32_t         m_totalObjects {0};
        uint64_t         m_byType[8] {}; // indexed by VriObjectType
        float            m_interval {0.5f};
        float            m_since {1e9f}; // forces a snapshot on the first Update
        bool             m_supported {true};
    };
} // namespace vrf::ui

#endif // VRF_WITH_IMGUI
