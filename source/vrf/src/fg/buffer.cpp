#include "vrf/fg/buffer.hpp"

#include <cassert>
#include <cstring>

#include "vrf/core/log.hpp"
#include "vrf/gpu/render_device.hpp"

namespace vrf::fg
{
    namespace
    {
        [[nodiscard]] VriBufferUsageFlags usageFor(const BufferType type)
        {
            switch (type)
            {
                case BufferType::UniformBuffer:
                    return VriBufferUsage_ConstantBuffer | VriBufferUsage_TransferDst;
                case BufferType::StorageBuffer:
                    return VriBufferUsage_StorageBuffer | VriBufferUsage_TransferSrc | VriBufferUsage_TransferDst;
                case BufferType::VertexBuffer:
                    return VriBufferUsage_VertexBuffer | VriBufferUsage_StorageBuffer | VriBufferUsage_TransferDst;
                case BufferType::IndexBuffer:
                    return VriBufferUsage_IndexBuffer | VriBufferUsage_StorageBuffer | VriBufferUsage_TransferDst;
            }
            return VriBufferUsage_None;
        }

        // Uniform (per-frame constant) buffers live host-visible so passes can
        // map-write them directly; everything else is device-local.
        [[nodiscard]] VriMemoryLocation locationFor(const BufferType type)
        {
            return type == BufferType::UniformBuffer ? VriMemoryLocation_HostUpload : VriMemoryLocation_Device;
        }
    } // namespace

    Buffer::Buffer(RenderDevice& device, VriBuffer* buffer, const Desc& desc) noexcept :
        m_device {&device}, m_buffer {buffer}, m_desc {desc}
    {}

    Buffer::~Buffer() { Reset(); }

    Buffer::Buffer(Buffer&& other) noexcept :
        state {other.state}, m_device {other.m_device}, m_buffer {other.m_buffer}, m_desc {other.m_desc},
        m_views {std::move(other.m_views)}
    {
        other.m_buffer = nullptr;
        other.m_views.clear();
    }

    Buffer& Buffer::operator=(Buffer&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            state          = other.state;
            m_device       = other.m_device;
            m_buffer       = other.m_buffer;
            m_desc         = other.m_desc;
            m_views        = std::move(other.m_views);
            other.m_buffer = nullptr;
            other.m_views.clear();
        }
        return *this;
    }

    Expected<Buffer> Buffer::Create(RenderDevice& device, const Desc& desc)
    {
        assert(desc.dataSize() > 0);

        const VriBufferDesc bd {
            .size            = desc.dataSize(),
            .structureStride = desc.type == BufferType::StorageBuffer ? desc.stride : 0,
            .usage           = usageFor(desc.type),
            .memoryLocation  = locationFor(desc.type),
        };

        VriBuffer* buffer = nullptr;
        if (const auto r = device.Core().CreateBuffer(device.Handle(), &bd, &buffer); !Succeeded(r))
        {
            return MakeError(r, "fg::Buffer", "CreateBuffer failed");
        }
        return Buffer {device, buffer, desc};
    }

    VriDescriptor* Buffer::View(const VriDescriptorType type)
    {
        if (const auto it = m_views.find(type); it != m_views.end())
        {
            return it->second;
        }

        const VriBufferViewDesc vd {
            .buffer   = m_buffer,
            .viewType = type,
            .format   = VriFormat_Unknown,
            .offset   = 0,
            .size     = 0, // whole buffer
        };

        VriDescriptor* view = nullptr;
        if (const auto r = m_device->Core().CreateBufferView(m_device->Handle(), &vd, &view); !Succeeded(r))
        {
            LogError("fg::Buffer: CreateBufferView failed ({})", ToString(r));
            return nullptr;
        }
        m_views.emplace(type, view);
        return view;
    }

    void Buffer::Write(const uint64_t offset, const uint64_t size, const void* data)
    {
        void* dst = m_device->Core().MapBuffer(m_buffer, offset, size);
        if (!dst)
        {
            LogError("fg::Buffer: MapBuffer failed (host-visible buffers only)");
            return;
        }
        std::memcpy(dst, data, size);
        m_device->Core().UnmapBuffer(m_buffer);
    }

    void Buffer::Reset() noexcept
    {
        if (m_device)
        {
            for (auto& [_, view] : m_views)
            {
                m_device->Core().DestroyDescriptor(view);
            }
            m_views.clear();
            if (m_buffer)
            {
                m_device->Core().DestroyBuffer(m_buffer);
            }
        }
        m_buffer = nullptr;
        m_device = nullptr;
    }
} // namespace vrf::fg
