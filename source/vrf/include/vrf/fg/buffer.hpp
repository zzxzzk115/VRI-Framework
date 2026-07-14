/*
 * buffer.hpp - the buffer object flowing through the framegraph.
 *
 * Mirrors fg::Texture: carries the buffer's last-known access/stage state for
 * VRI's explicit barriers plus a cached view descriptor per view type, and a
 * persistent mapping for host-visible buffers (per-frame uploads).
 */
#pragma once

#include <cstdint>
#include <unordered_map>

#include <vri/vri.h>

#include "vrf/core/result.hpp"

namespace vrf
{
    class RenderDevice;
}

namespace vrf::fg
{
    enum class BufferType : uint32_t
    {
        UniformBuffer,
        StorageBuffer,
        VertexBuffer,
        IndexBuffer,
    };

    class Buffer
    {
    public:
        struct Desc
        {
            BufferType type {BufferType::UniformBuffer};
            uint32_t   stride {1};
            uint64_t   capacity {0};

            [[nodiscard]] constexpr uint64_t dataSize() const { return stride * capacity; }
            [[nodiscard]] bool               operator==(const Desc&) const = default;
        };

        Buffer() = default;
        ~Buffer();

        Buffer(const Buffer&)            = delete;
        Buffer& operator=(const Buffer&) = delete;
        Buffer(Buffer&& other) noexcept;
        Buffer& operator=(Buffer&& other) noexcept;

        [[nodiscard]] static Expected<Buffer> Create(RenderDevice&, const Desc&);

        [[nodiscard]] VriBuffer*  Handle() const noexcept { return m_buffer; }
        [[nodiscard]] const Desc& GetDesc() const noexcept { return m_desc; }
        [[nodiscard]] uint64_t    GetSize() const noexcept { return m_desc.dataSize(); }
        [[nodiscard]] explicit    operator bool() const noexcept { return m_buffer != nullptr; }

        // View descriptor for shader binding (ConstantBuffer / StorageBuffer).
        [[nodiscard]] VriDescriptor* View(VriDescriptorType);

        // Host-visible write (uniform/storage transient uploads). Maps, copies,
        // unmaps. Only valid for buffers created HostUpload (uniform buffers are).
        void Write(uint64_t offset, uint64_t size, const void* data);

        // Last-known whole-resource state, updated by the barrier helpers.
        VriAccessStage state {VriAccess_None, VriPipelineStage_AllCommands};

    private:
        Buffer(RenderDevice& device, VriBuffer* buffer, const Desc& desc) noexcept;

        void Reset() noexcept;

        RenderDevice* m_device = nullptr;
        VriBuffer*    m_buffer = nullptr;
        Desc          m_desc {};

        std::unordered_map<uint32_t, VriDescriptor*> m_views;
    };
} // namespace vrf::fg
