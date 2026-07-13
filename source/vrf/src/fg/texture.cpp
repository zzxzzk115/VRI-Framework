#include "vrf/fg/texture.hpp"

#include "vrf/core/log.hpp"
#include "vrf/gpu/render_device.hpp"

namespace vrf::fg
{
    namespace
    {
        [[nodiscard]] VriTextureType textureType(const Texture::Desc& desc)
        {
            if (desc.cubemap)
            {
                return desc.layers > 1 ? VriTextureType_CubeArray : VriTextureType_Cube;
            }
            if (desc.depth > 0)
            {
                return VriTextureType_3D;
            }
            return desc.layers > 1 ? VriTextureType_2DArray : VriTextureType_2D;
        }

        [[nodiscard]] VriTextureViewType wholeViewType(const Texture::Desc& desc)
        {
            if (desc.cubemap)
            {
                return desc.layers > 1 ? VriTextureViewType_CubeArray : VriTextureViewType_Cube;
            }
            if (desc.depth > 0)
            {
                return VriTextureViewType_3D;
            }
            return desc.layers > 1 ? VriTextureViewType_2DArray : VriTextureViewType_2D;
        }
    } // namespace

    Texture::Texture(RenderDevice& device, VriTexture* texture, const Desc& desc, const bool owned) noexcept :
        m_device {&device}, m_texture {texture}, m_desc {desc}, m_owned {owned}
    {}

    Texture::~Texture() { Reset(); }

    Texture::Texture(Texture&& other) noexcept :
        state {other.state}, m_device {other.m_device}, m_texture {other.m_texture}, m_desc {other.m_desc},
        m_owned {other.m_owned}, m_views {std::move(other.m_views)}
    {
        other.m_texture = nullptr;
        other.m_views.clear();
    }

    Texture& Texture::operator=(Texture&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            state           = other.state;
            m_device        = other.m_device;
            m_texture       = other.m_texture;
            m_desc          = other.m_desc;
            m_owned         = other.m_owned;
            m_views         = std::move(other.m_views);
            other.m_texture = nullptr;
            other.m_views.clear();
        }
        return *this;
    }

    Expected<Texture> Texture::Create(RenderDevice& device, const Desc& desc)
    {
        const VriTextureDesc td {
            .type           = textureType(desc),
            .format         = desc.format,
            .width          = desc.extent.width,
            .height         = desc.extent.height,
            .depth          = std::max(desc.depth, 1u),
            .mipNum         = std::max(desc.numMipLevels, 1u),
            .layerNum       = (desc.cubemap ? 6u : 1u) * std::max(desc.layers, 1u),
            .sampleNum      = 1,
            .usage          = desc.usage,
            .memoryLocation = VriMemoryLocation_Device,
        };

        VriTexture* texture = nullptr;
        if (const auto r = device.Core().CreateTexture(device.Handle(), &td, &texture); !Succeeded(r))
        {
            return MakeError(r, "fg::Texture", "CreateTexture failed");
        }
        return Texture {device, texture, desc, true};
    }

    Texture Texture::Borrow(RenderDevice&               device,
                            VriTexture*                 texture,
                            const Desc&                 desc,
                            const VriAccessLayoutStage& state)
    {
        Texture out {device, texture, desc, false};
        out.state = state;
        return out;
    }

    VriDescriptor* Texture::AttachmentView(const ImageAspect             aspect,
                                           const std::optional<uint32_t> layer,
                                           const std::optional<CubeFace> face)
    {
        // Cube faces are laid out as consecutive layers.
        std::optional<uint32_t> resolvedLayer = layer;
        if (face)
        {
            resolvedLayer = (layer.value_or(0) * 6u) + static_cast<uint32_t>(*face);
        }

        const uint64_t key = (uint64_t {1} << 60) | (uint64_t {static_cast<uint32_t>(aspect)} << 32) |
                             (resolvedLayer ? *resolvedLayer + 1u : 0u);

        const auto layerNum = (m_desc.cubemap ? 6u : 1u) * std::max(m_desc.layers, 1u);
        return GetOrCreateView(key,
                               VriTextureViewDesc {
                                   .texture   = m_texture,
                                   .viewType  = resolvedLayer || layerNum == 1 ? VriTextureViewType_2D :
                                                                                 VriTextureViewType_2DArray,
                                   .format    = VriFormat_Unknown,
                                   .aspect    = convert(aspect),
                                   .baseMip   = 0,
                                   .mipNum    = 1,
                                   .baseLayer = resolvedLayer.value_or(0),
                                   .layerNum  = resolvedLayer ? 1u : 0u,
                               });
    }

    VriDescriptor* Texture::SampledView(const ImageAspect aspect)
    {
        const uint64_t key = (uint64_t {2} << 60) | static_cast<uint32_t>(aspect);
        return GetOrCreateView(key,
                               VriTextureViewDesc {
                                   .texture  = m_texture,
                                   .viewType = wholeViewType(m_desc),
                                   .format   = VriFormat_Unknown,
                                   .aspect   = convert(aspect),
                               });
    }

    VriDescriptor* Texture::StorageView(const uint32_t mipLevel)
    {
        const uint64_t key = (uint64_t {3} << 60) | mipLevel;
        return GetOrCreateView(key,
                               VriTextureViewDesc {
                                   .texture  = m_texture,
                                   .viewType = wholeViewType(m_desc),
                                   .format   = VriFormat_Unknown,
                                   .aspect   = VriImageAspect_Color,
                                   .baseMip  = mipLevel,
                                   .mipNum   = 1,
                               });
    }

    VriDescriptor* Texture::GetOrCreateView(const uint64_t key, const VriTextureViewDesc& desc)
    {
        if (const auto it = m_views.find(key); it != m_views.end())
        {
            return it->second;
        }

        VriDescriptor* view = nullptr;
        if (const auto r = m_device->Core().CreateTextureView(m_device->Handle(), &desc, &view);
            r != VriResult_Success)
        {
            LogError("fg::Texture: CreateTextureView failed ({})", static_cast<int>(r));
            return nullptr;
        }
        m_views.emplace(key, view);
        return view;
    }

    void Texture::DestroyViews() noexcept
    {
        for (auto& [_, view] : m_views)
        {
            m_device->Core().DestroyDescriptor(view);
        }
        m_views.clear();
    }

    void Texture::Reset() noexcept
    {
        if (m_device)
        {
            DestroyViews();
            if (m_owned && m_texture)
            {
                m_device->Core().DestroyTexture(m_texture);
            }
        }
        m_texture = nullptr;
        m_device  = nullptr;
    }
} // namespace vrf::fg
