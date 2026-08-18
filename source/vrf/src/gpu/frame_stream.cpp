#include "vrf/gpu/frame_stream.hpp"

#include <utility>

#include "vrf/core/profiling.hpp"
#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    FrameStream::FrameStream(FrameStream&& other) noexcept :
        m_device(std::exchange(other.m_device, nullptr)), m_slots(std::move(other.m_slots)),
        m_fence(std::exchange(other.m_fence, nullptr)), m_nextValue(std::exchange(other.m_nextValue, 0)),
        m_slotIndex(std::exchange(other.m_slotIndex, 0)), m_queueType(other.m_queueType)
    {
        other.m_slots.clear();
    }

    FrameStream& FrameStream::operator=(FrameStream&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_device    = std::exchange(other.m_device, nullptr);
            m_slots     = std::move(other.m_slots);
            m_fence     = std::exchange(other.m_fence, nullptr);
            m_nextValue = std::exchange(other.m_nextValue, 0);
            m_slotIndex = std::exchange(other.m_slotIndex, 0);
            m_queueType = other.m_queueType;
            other.m_slots.clear();
        }
        return *this;
    }

    FrameStream::~FrameStream() { Reset(); }

    void FrameStream::Reset() noexcept
    {
        if (!m_device)
            return;
        WaitAll();
        const VriCoreInterface& c = m_device->Core();
        for (auto& slot : m_slots)
        {
            if (slot.alloc)
                c.DestroyCommandAllocator(slot.alloc);
        }
        if (m_fence)
            c.DestroyFence(m_fence);
        m_slots.clear();
        m_fence  = nullptr;
        m_device = nullptr;
    }

    Expected<FrameStream>
    FrameStream::Create(RenderDevice& device, const uint32_t framesInFlight, const VriQueueType queueType)
    {
        const VriCoreInterface& c = device.Core();

        FrameStream stream;
        stream.m_device    = &device;
        stream.m_queueType = queueType;
        stream.m_slots.resize(std::max(framesInFlight, 1u));

        for (auto& slot : stream.m_slots)
        {
            VriResult r = c.CreateCommandAllocator(device.Handle(), queueType, &slot.alloc);
            if (r != VriResult_Success)
                return MakeError(r, "FrameStream::Create", "CreateCommandAllocator failed");

            r = c.CreateCommandBuffer(slot.alloc, &slot.cmd);
            if (r != VriResult_Success)
                return MakeError(r, "FrameStream::Create", "CreateCommandBuffer failed");
        }

        if (const VriResult r = c.CreateFence(device.Handle(), 0, &stream.m_fence); r != VriResult_Success)
            return MakeError(r, "FrameStream::Create", "CreateFence failed");

        return stream;
    }

    VriCommandBuffer* FrameStream::Begin()
    {
        VRF_ZONE("FrameStream::Begin");
        const VriCoreInterface& c    = m_device->Core();
        Slot&                   slot = m_slots[m_slotIndex];

        // Wait only for this slot's own previous submission - the other
        // framesInFlight-1 frames stay in flight.
        if (slot.submittedValue != 0)
        {
            VRF_ZONE("FrameStream::WaitSlot");
            c.Wait(m_fence, slot.submittedValue);
        }

        c.ResetCommandAllocator(slot.alloc);
        c.BeginCommandBuffer(slot.cmd);
        return slot.cmd;
    }

    void FrameStream::Submit(const VriFenceSubmitDesc* waits, const uint32_t waitNum)
    {
        VRF_ZONE("FrameStream::Submit");
        VRF_FRAME_MARK();
        const VriCoreInterface& c    = m_device->Core();
        Slot&                   slot = m_slots[m_slotIndex];

        c.EndCommandBuffer(slot.cmd);

        VriFenceSubmitDesc signal {};
        signal.fence  = m_fence;
        signal.value  = ++m_nextValue;
        signal.stages = VriPipelineStage_AllCommands;

        VriQueueSubmitDesc submit {};
        submit.waitFences       = waitNum ? waits : nullptr;
        submit.waitFenceNum     = waitNum;
        submit.commandBuffers   = &slot.cmd;
        submit.commandBufferNum = 1;
        submit.signalFences     = &signal;
        submit.signalFenceNum   = 1;

        c.QueueSubmit(m_device->Queue(m_queueType), &submit);
        slot.submittedValue = m_nextValue;

        m_slotIndex = (m_slotIndex + 1) % static_cast<uint32_t>(m_slots.size());
    }

    void FrameStream::WaitAll()
    {
        if (m_device && m_fence && m_nextValue != 0)
        {
            m_device->Core().Wait(m_fence, m_nextValue);
        }
    }
} // namespace vrf
