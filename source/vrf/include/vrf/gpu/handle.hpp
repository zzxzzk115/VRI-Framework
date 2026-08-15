/*
 * handle.hpp - move-only RAII ownership for raw VRI resource handles.
 *
 * VRI hands back raw pointers (vrf::BufferHandle, vrf::TextureHandle, ... - the VriBuffer/VriTexture
 * handle aliases from gpu/vri_types.hpp) that the owner must Destroy() by hand - the one lifetime
 * model in vrf that is not RAII (builder outputs, GpuMesh,
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

#include "vrf/gpu/vri_types.hpp"
#include <vri/vri.h>

#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    // How each VRI handle type is destroyed (all via the device's core interface table).
    template<class T>
    struct VriResourceTraits;

    template<>
    struct VriResourceTraits<BufferHandle>
    {
        static void Destroy(RenderDevice& d, BufferHandle* h) noexcept { d.Core().DestroyBuffer(h); }
    };
    template<>
    struct VriResourceTraits<TextureHandle>
    {
        static void Destroy(RenderDevice& d, TextureHandle* h) noexcept { d.Core().DestroyTexture(h); }
    };
    template<>
    struct VriResourceTraits<DescriptorHandle>
    {
        static void Destroy(RenderDevice& d, DescriptorHandle* h) noexcept { d.Core().DestroyDescriptor(h); }
    };
    template<>
    struct VriResourceTraits<PipelineHandle>
    {
        static void Destroy(RenderDevice& d, PipelineHandle* h) noexcept { d.Core().DestroyPipeline(h); }
    };
    template<>
    struct VriResourceTraits<PipelineLayoutHandle>
    {
        static void Destroy(RenderDevice& d, PipelineLayoutHandle* h) noexcept { d.Core().DestroyPipelineLayout(h); }
    };
    template<>
    struct VriResourceTraits<DescriptorPoolHandle>
    {
        static void Destroy(RenderDevice& d, DescriptorPoolHandle* h) noexcept { d.Core().DestroyDescriptorPool(h); }
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

    using UniqueBuffer         = Unique<BufferHandle>;
    using UniqueTexture        = Unique<TextureHandle>;
    using UniqueDescriptor     = Unique<DescriptorHandle>; // views + samplers
    using UniquePipeline       = Unique<PipelineHandle>;
    using UniquePipelineLayout = Unique<PipelineLayoutHandle>;
    using UniqueDescriptorPool = Unique<DescriptorPoolHandle>;
} // namespace vrf
