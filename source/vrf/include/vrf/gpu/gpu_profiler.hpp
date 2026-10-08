/*
 * gpu_profiler.hpp - GPU timing via VRI timestamp queries (vri_ext_query.h).
 *
 * A capability the app composes into its own frame loop - deliberately decoupled from FrameStream
 * (vrf provides the capability; the app drives it with the FrameStream slot index):
 *
 *   auto* cmd = frames.Begin();
 *   profiler.BeginFrame(cmd, frames.FrameIndex());   // resolves results from framesInFlight frames ago
 *   { VRF_GPU_ZONE(profiler, cmd, "gbuffer"); ...record the pass... }   // nestable
 *   profiler.EndFrame(cmd);
 *   frames.Submit();
 *   for (const auto& z : profiler.Results()) ...      // last completed frame's spans, in record order
 *
 * Results lag by framesInFlight frames (read back only after the slot's fence, which
 * FrameStream::Begin already waited on - so no extra sync). BeginFrame/EndFrame must be called
 * OUTSIDE any render pass (they reset/copy query pools). Timestamp collection is disabled (Enabled()==false) on devices
 * without timestamp queries; zone methods still emit tool markers.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vri/ext/vri_ext_query.h>
#include <vri/vri.h>

#include "vrf/core/result.hpp"

namespace vrf
{
    class RenderDevice;

    class GpuProfiler
    {
    public:
        struct Zone
        {
            std::string name;
            double      milliseconds {0.0};
            uint32_t    depth {0}; // nesting level (0 = outermost)
        };

        GpuProfiler() = default;
        ~GpuProfiler();
        GpuProfiler(const GpuProfiler&)            = delete;
        GpuProfiler& operator=(const GpuProfiler&) = delete;
        GpuProfiler(GpuProfiler&&) noexcept;
        GpuProfiler& operator=(GpuProfiler&&) noexcept;

        // Succeeds even when the device lacks timestamp queries - the result is a disabled
        // timestamp profiler; tool markers are still emitted by BeginZone/EndZone.
        [[nodiscard]] static Expected<GpuProfiler>
        Create(RenderDevice&, uint32_t framesInFlight, uint32_t maxZonesPerFrame = 64, bool collectTimestamps = true);

        // Reports timestamp collection only; markers work when this returns false.
        [[nodiscard]] bool Enabled() const noexcept { return m_device != nullptr; }

        // Reset this slot's pool and resolve the results captured the last time this slot ran.
        void BeginFrame(VriCommandBuffer* cmd, uint32_t frameIndex);
        void BeginZone(VriCommandBuffer* cmd, const char* name);
        void EndZone(VriCommandBuffer* cmd);
        void EndFrame(VriCommandBuffer* cmd);

        // Spans resolved at the last BeginFrame; stable until the next BeginFrame, in record order.
        [[nodiscard]] const std::vector<Zone>& Results() const noexcept { return m_results; }
        // Sum of the outermost spans of the last resolved frame (total GPU time).
        [[nodiscard]] double LastFrameMs() const noexcept;

    private:
        void Destroy() noexcept;

        struct Record
        {
            std::string name;
            uint32_t    begin {0};
            uint32_t    end {0xFFFFFFFFu}; // unset until EndZone
            uint32_t    depth {0};
        };
        struct Slot
        {
            VriQueryPool*       pool {nullptr};
            VriBuffer*          readback {nullptr};
            std::vector<Record> records;         // zones recorded into this slot this cycle (CPU side)
            uint32_t            next {0};        // next free query index
            bool                pending {false}; // has unresolved results copied by a prior submit
        };

        RenderDevice*         m_markerDevice {nullptr};
        uint32_t              m_markerDepth {0};
        RenderDevice*         m_device {nullptr};
        VriQueryInterface     m_query {};
        double                m_periodNs {0.0};
        uint32_t              m_maxQueries {0};
        std::vector<Slot>     m_slots;
        uint32_t              m_current {0};
        std::vector<uint32_t> m_stack; // open-zone record indices (UINT32_MAX = skipped/overflow)
        std::vector<Zone>     m_results;
    };

    // A tool marker independent of timestamp support, profiler creation and build mode.
    // The device/interface and command buffer must outlive the scope.
    class GpuDebugGroup
    {
    public:
        GpuDebugGroup(RenderDevice& device, VriCommandBuffer* cmd, const char* name);
        GpuDebugGroup(const VriCoreInterface& core, VriCommandBuffer* cmd, const char* name)
        {
            if (cmd && core.CmdBeginDebugGroup && core.CmdEndDebugGroup)
            {
                m_core = &core;
                m_cmd  = cmd;
                core.CmdBeginDebugGroup(cmd, name ? name : "");
            }
        }
        ~GpuDebugGroup()
        {
            if (m_core)
                m_core->CmdEndDebugGroup(m_cmd);
        }
        GpuDebugGroup(const GpuDebugGroup&)            = delete;
        GpuDebugGroup& operator=(const GpuDebugGroup&) = delete;

    private:
        const VriCoreInterface* m_core {nullptr};
        VriCommandBuffer*       m_cmd {nullptr};
    };

    // RAII scope for a GPU zone (mirrors core/profiling.hpp's CPU VRF_ZONE).
    struct GpuZoneScope
    {
        GpuZoneScope(GpuProfiler& p, VriCommandBuffer* cmd, const char* name) : profiler {p}, cmd {cmd}
        {
            profiler.BeginZone(cmd, name);
        }
        ~GpuZoneScope() { profiler.EndZone(cmd); }
        GpuZoneScope(const GpuZoneScope&)            = delete;
        GpuZoneScope& operator=(const GpuZoneScope&) = delete;

        GpuProfiler&      profiler;
        VriCommandBuffer* cmd;
    };
} // namespace vrf

#define VRF_GPU_ZONE_CONCAT2(a, b) a##b
#define VRF_GPU_ZONE_CONCAT(a, b) VRF_GPU_ZONE_CONCAT2(a, b)
#define VRF_GPU_ZONE(profiler, cmd, name) \
    ::vrf::GpuZoneScope VRF_GPU_ZONE_CONCAT(_vrfGpuZone_, __LINE__) { (profiler), (cmd), (name) }
