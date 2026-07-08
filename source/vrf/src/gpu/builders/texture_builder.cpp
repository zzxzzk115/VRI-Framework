#include "vrf/gpu/builders/texture_builder.hpp"

#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    TextureBuilder::TextureBuilder()
    {
        m_desc.type           = VriTextureType_2D;
        m_desc.format         = VriFormat_Unknown;
        m_desc.width          = 1;
        m_desc.height         = 1;
        m_desc.depth          = 1;
        m_desc.mipNum         = 1;
        m_desc.layerNum       = 1;
        m_desc.sampleNum      = 1;
        m_desc.usage          = VriTextureUsage_ShaderResource;
        m_desc.memoryLocation = VriMemoryLocation_Device;
    }

    Expected<VriTexture*> TextureBuilder::Build(RenderDevice& device) const
    {
        VriTexture*     texture = nullptr;
        const VriResult r       = device.Core().CreateTexture(device.Handle(), &m_desc, &texture);
        if (r != VriResult_Success)
            return MakeError(r, "TextureBuilder::Build", "CreateTexture failed");
        return texture;
    }
} // namespace vrf
