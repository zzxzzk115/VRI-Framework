#include "vrf/gpu/frame.hpp"

#include <utility>

#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    Frame::Frame(Frame&& other) noexcept :
        m_device(std::exchange(other.m_device, nullptr)), m_alloc(std::exchange(other.m_alloc, nullptr)),
        m_cmd(std::exchange(other.m_cmd, nullptr)), m_fence(std::exchange(other.m_fence, nullptr)),
        m_value(std::exchange(other.m_value, 0))
    {}

    Frame& Frame::operator=(Frame&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_device = std::exchange(other.m_device, nullptr);
            m_alloc  = std::exchange(other.m_alloc, nullptr);
            m_cmd    = std::exchange(other.m_cmd, nullptr);
            m_fence  = std::exchange(other.m_fence, nullptr);
            m_value  = std::exchange(other.m_value, 0);
        }
        return *this;
    }

    Frame::~Frame() { Reset(); }

    void Frame::Reset() noexcept
    {
        if (!m_device)
            return;
        const VriCoreInterface& c = m_device->Core();
        if (m_fence)
            c.DestroyFence(m_fence);
        if (m_alloc)
            c.DestroyCommandAllocator(m_alloc);
        m_fence = nullptr;
        m_alloc = nullptr;
        m_cmd   = nullptr;
    }

    Expected<Frame> Frame::Create(RenderDevice& device)
    {
        const VriCoreInterface& c = device.Core();

        VriCommandAllocator* alloc = nullptr;
        VriResult            r     = c.CreateCommandAllocator(device.Handle(), VriQueueType_Graphics, &alloc);
        if (r != VriResult_Success)
            return MakeError(r, "Frame::Create", "CreateCommandAllocator failed");

        VriCommandBuffer* cmd = nullptr;
        r                     = c.CreateCommandBuffer(alloc, &cmd);
        if (r != VriResult_Success)
        {
            c.DestroyCommandAllocator(alloc);
            return MakeError(r, "Frame::Create", "CreateCommandBuffer failed");
        }

        VriFence* fence = nullptr;
        r               = c.CreateFence(device.Handle(), 0, &fence);
        if (r != VriResult_Success)
        {
            c.DestroyCommandAllocator(alloc);
            return MakeError(r, "Frame::Create", "CreateFence failed");
        }

        Frame frame;
        frame.m_device = &device;
        frame.m_alloc  = alloc;
        frame.m_cmd    = cmd;
        frame.m_fence  = fence;
        frame.m_value  = 0;
        return frame;
    }

    VriCommandBuffer* Frame::Begin()
    {
        const VriCoreInterface& c = m_device->Core();
        c.ResetCommandAllocator(m_alloc);
        c.BeginCommandBuffer(m_cmd);
        return m_cmd;
    }

    void Frame::Submit()
    {
        const VriCoreInterface& c = m_device->Core();
        c.EndCommandBuffer(m_cmd);

        VriFenceSubmitDesc signal {};
        signal.fence  = m_fence;
        signal.value  = ++m_value;
        signal.stages = VriPipelineStage_AllCommands;

        VriQueueSubmitDesc submit {};
        submit.commandBuffers   = &m_cmd;
        submit.commandBufferNum = 1;
        submit.signalFences     = &signal;
        submit.signalFenceNum   = 1;

        c.QueueSubmit(m_device->GraphicsQueue(), &submit);
        c.Wait(m_fence, m_value);
    }
} // namespace vrf
