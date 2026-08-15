/*
 * frame.hpp - per-frame command machinery: a command allocator + command buffer
 * + timeline fence, plus the monotonically increasing fence value.
 *
 * Begin() resets the allocator and opens the command buffer; record into Cmd();
 * Submit() closes it, submits with a timeline signal, and waits for completion
 * (a simple synchronous, one-frame-in-flight loop suitable for the framework's
 * default present path).
 */
#pragma once

#include <cstdint>

#include "vrf/gpu/vri_types.hpp"
#include <vri/vri.h>

#include "vrf/core/result.hpp"

namespace vrf
{
    class RenderDevice;

    class Frame
    {
    public:
        Frame() = default;
        ~Frame();

        Frame(const Frame&)            = delete;
        Frame& operator=(const Frame&) = delete;
        Frame(Frame&& other) noexcept;
        Frame& operator=(Frame&& other) noexcept;

        [[nodiscard]] static Expected<Frame> Create(RenderDevice& device);

        // Reset the allocator and begin recording; returns the command buffer.
        [[nodiscard]] CommandBufferHandle* Begin();

        // End recording, submit (signalling the timeline fence), and wait for the GPU.
        void Submit();

        [[nodiscard]] CommandBufferHandle* Cmd() const noexcept { return m_cmd; }
        [[nodiscard]] FenceHandle*         Fence() const noexcept { return m_fence; }
        [[nodiscard]] uint64_t             Value() const noexcept { return m_value; }

    private:
        void Reset() noexcept;

        RenderDevice*           m_device = nullptr;
        CommandAllocatorHandle* m_alloc  = nullptr;
        CommandBufferHandle*    m_cmd    = nullptr;
        FenceHandle*            m_fence  = nullptr;
        uint64_t                m_value  = 0;
    };
} // namespace vrf
