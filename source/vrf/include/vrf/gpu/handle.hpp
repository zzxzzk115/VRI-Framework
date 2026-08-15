/*
 * handle.hpp - move-only RAII ownership for raw VRI resource handles.
 *
 * VRI hands back raw pointers (VriBuffer, VriTexture, VriPipeline, ...) that the owner must
 * Destroy() by hand - the one lifetime model in vrf that is not RAII (builder outputs, GpuMesh,
 * GpuTexture). Unique<T> wraps such a handle with the matching core-interface destroy call, so
 * ownership is uniform: it frees on scope exit, moves transfer it, and it converts implicitly to
 * the raw T* for passing straight into VRI calls.
 *
 *   vrf::UniqueBuffer buf {device, *BufferBuilder{}...Build(device)};
 *   core.CmdSetVertexBuffers(cmd, 0, 1, &*buf, ...);   // implicit operator T*
 *   // freed automatically; no buf.Destroy(device)
 */
#pragma once

#include <utility>

#include <vri/vri.h>

#include "vrf/gpu/render_device.hpp"
#include "vrf/gpu/rhi.hpp"

namespace vrf
{
    // How each VRI handle type is destroyed (all via the device's core interface table).
    template<class T>
    struct VriResourceTraits;

    template<>
    struct VriResourceTraits<rhi::Buffer>
    {
        static void Destroy(RenderDevice& d, rhi::Buffer* h) noexcept { d.Core().DestroyBuffer(h); }
    };
    template<>
    struct VriResourceTraits<rhi::Texture>
    {
        static void Destroy(RenderDevice& d, rhi::Texture* h) noexcept { d.Core().DestroyTexture(h); }
    };
    template<>
    struct VriResourceTraits<rhi::Descriptor>
    {
        static void Destroy(RenderDevice& d, rhi::Descriptor* h) noexcept { d.Core().DestroyDescriptor(h); }
    };
    template<>
    struct VriResourceTraits<rhi::Pipeline>
    {
        static void Destroy(RenderDevice& d, rhi::Pipeline* h) noexcept { d.Core().DestroyPipeline(h); }
    };
    template<>
    struct VriResourceTraits<rhi::PipelineLayout>
    {
        static void Destroy(RenderDevice& d, rhi::PipelineLayout* h) noexcept { d.Core().DestroyPipelineLayout(h); }
    };
    template<>
    struct VriResourceTraits<rhi::DescriptorPool>
    {
        static void Destroy(RenderDevice& d, rhi::DescriptorPool* h) noexcept { d.Core().DestroyDescriptorPool(h); }
    };

    template<class T>
    class Unique
    {
    public:
        Unique() = default;
        Unique(RenderDevice& device, T* handle) noexcept : m_device {&device}, m_handle {handle} {}

        ~Unique() { reset(); }

        Unique(const Unique&)            = delete;
        Unique& operator=(const Unique&) = delete;

        Unique(Unique&& other) noexcept :
            m_device {std::exchange(other.m_device, nullptr)}, m_handle {std::exchange(other.m_handle, nullptr)}
        {}
        Unique& operator=(Unique&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_device = std::exchange(other.m_device, nullptr);
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        [[nodiscard]] T* get() const noexcept { return m_handle; }
        [[nodiscard]] T* operator*() const noexcept { return m_handle; }
        [[nodiscard]] operator T*() const noexcept { return m_handle; } // NOLINT: implicit for VRI calls
        [[nodiscard]] explicit operator bool() const noexcept { return m_handle != nullptr; }

        // Relinquish ownership (caller now frees it).
        [[nodiscard]] T* release() noexcept { return std::exchange(m_handle, nullptr); }

        void reset() noexcept
        {
            if (m_handle && m_device)
            {
                VriResourceTraits<T>::Destroy(*m_device, m_handle);
            }
            m_handle = nullptr;
        }

    private:
        RenderDevice* m_device {nullptr};
        T*            m_handle {nullptr};
    };

    using UniqueBuffer         = Unique<rhi::Buffer>;
    using UniqueTexture        = Unique<rhi::Texture>;
    using UniqueDescriptor     = Unique<rhi::Descriptor>; // views + samplers
    using UniquePipeline       = Unique<rhi::Pipeline>;
    using UniquePipelineLayout = Unique<rhi::PipelineLayout>;
    using UniqueDescriptorPool = Unique<rhi::DescriptorPool>;
} // namespace vrf
