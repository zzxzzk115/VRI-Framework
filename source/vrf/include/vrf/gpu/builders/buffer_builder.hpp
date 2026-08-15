/*
 * buffer_builder.hpp - fluent builder over VriBufferDesc.
 */
#pragma once

#include <cstdint>

#include "vrf/gpu/vri_types.hpp"
#include <vri/vri.h>

#include "vrf/core/result.hpp"

namespace vrf
{
    class RenderDevice;

    class BufferBuilder
    {
    public:
        BufferBuilder& SetSize(uint64_t size)
        {
            m_desc.size = size;
            return *this;
        }

        BufferBuilder& SetUsage(VriBufferUsageFlags usage)
        {
            m_desc.usage = usage;
            return *this;
        }

        BufferBuilder& SetMemoryLocation(MemoryLocation location)
        {
            m_desc.memoryLocation = location;
            return *this;
        }

        BufferBuilder& SetStructureStride(uint32_t stride)
        {
            m_desc.structureStride = stride;
            return *this;
        }

        [[nodiscard]] Expected<BufferHandle*> Build(RenderDevice& device) const;

    private:
        VriBufferDesc m_desc {};
    };
} // namespace vrf
