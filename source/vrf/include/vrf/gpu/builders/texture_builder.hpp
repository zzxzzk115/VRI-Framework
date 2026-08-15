/*
 * texture_builder.hpp - fluent builder over VriTextureDesc.
 */
#pragma once

#include <cstdint>

#include "vrf/gpu/vri_types.hpp"
#include <vri/vri.h>

#include "vrf/core/math.hpp"
#include "vrf/core/result.hpp"

namespace vrf
{
    class RenderDevice;

    class TextureBuilder
    {
    public:
        TextureBuilder();

        TextureBuilder& SetType(TextureType type)
        {
            m_desc.type = type;
            return *this;
        }

        TextureBuilder& SetFormat(VriFormat format)
        {
            m_desc.format = format;
            return *this;
        }

        TextureBuilder& SetExtent(uint32_t width, uint32_t height, uint32_t depth = 1)
        {
            m_desc.width  = width;
            m_desc.height = height;
            m_desc.depth  = depth;
            return *this;
        }

        TextureBuilder& SetExtent(Extent2D extent)
        {
            m_desc.width  = extent.width;
            m_desc.height = extent.height;
            m_desc.depth  = 1;
            return *this;
        }

        TextureBuilder& SetMipLevels(uint32_t mipNum)
        {
            m_desc.mipNum = mipNum;
            return *this;
        }

        TextureBuilder& SetLayers(uint32_t layerNum)
        {
            m_desc.layerNum = layerNum;
            return *this;
        }

        TextureBuilder& SetSampleCount(uint32_t sampleNum)
        {
            m_desc.sampleNum = sampleNum;
            return *this;
        }

        TextureBuilder& SetUsage(VriTextureUsageFlags usage)
        {
            m_desc.usage = usage;
            return *this;
        }

        TextureBuilder& SetMemoryLocation(MemoryLocation location)
        {
            m_desc.memoryLocation = location;
            return *this;
        }

        [[nodiscard]] Expected<TextureHandle*> Build(RenderDevice& device) const;

    private:
        VriTextureDesc m_desc {};
    };
} // namespace vrf
