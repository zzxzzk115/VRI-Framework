/*
 * frame_stream.hpp - N-frames-in-flight command machinery.
 *
 * The synchronous Frame (frame.hpp) submits and waits every frame - fine for a
 * simple present loop, inadequate when the CPU should record frame N+1 while
 * the GPU draws frame N (desktop pipelining, XR frame loops paced by
 * xrWaitFrame). FrameStream keeps one command allocator+buffer per slot and a
 * single timeline fence: Begin() waits only for the value this slot signalled
 * on its previous use, Submit() signals and returns without blocking.
 *
 * FrameIndex() cycles 0..framesInFlight-1; feed it to per-frame resources
 * (transient rings, descriptor allocator pools) that must not be recycled
 * while a submitted frame still reads them.
 */
#pragma once

#include <cstdint>
#include <vector>

#include <vri/vri.h>

#include "vrf/core/result.hpp"

namespace vrf
{
    class RenderDevice;

    class FrameStream
    {
    public:
        FrameStream() = default;
        ~FrameStream();

        FrameStream(const FrameStream&)            = delete;
        FrameStream& operator=(const FrameStream&) = delete;
        FrameStream(FrameStream&& other) noexcept;
        FrameStream& operator=(FrameStream&& other) noexcept;

        [[nodiscard]] static Expected<FrameStream> Create(RenderDevice& device, uint32_t framesInFlight = 2);

        // Wait until this slot's previous submission completed (no-op on first
        // use), reset its allocator, and open its command buffer.
        [[nodiscard]] VriCommandBuffer* Begin();

        // Close and submit the current slot's command buffer, signalling the
        // timeline. Does NOT wait; advances to the next slot.
        void Submit();

        // Block until every submitted frame completed (shutdown / resize).
        void WaitAll();

        [[nodiscard]] uint32_t FrameIndex() const noexcept { return m_slotIndex; }
        [[nodiscard]] uint32_t FramesInFlight() const noexcept { return static_cast<uint32_t>(m_slots.size()); }
        [[nodiscard]] VriCommandBuffer* Cmd() const noexcept
        {
            return m_slots.empty() ? nullptr : m_slots[m_slotIndex].cmd;
        }

    private:
        void Reset() noexcept;

        struct Slot
        {
            VriCommandAllocator* alloc {nullptr};
            VriCommandBuffer*    cmd {nullptr};
            uint64_t             submittedValue {0}; // timeline value signalled by this slot's last submit
        };

        RenderDevice*     m_device = nullptr;
        std::vector<Slot> m_slots;
        VriFence*         m_fence     = nullptr;
        uint64_t          m_nextValue = 0;
        uint32_t          m_slotIndex = 0;
    };
} // namespace vrf
