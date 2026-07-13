/*
 * texture.hpp - the texture object flowing through the framegraph.
 *
 * VRI barriers are fully explicit (before + after state), so unlike an
 * implicitly-tracked RHI this wrapper carries the texture's last-known
 * access/layout/stage state and a cache of the views (VriDescriptor) the
 * framegraph needs: attachment views per subresource, one sampled view per
 * aspect, and storage views per mip.
 *
 * A Texture either owns its VriTexture (transient-pool path) or borrows an
 * external one (imported swapchain / XR images).
 */
#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

#include <vri/vri.h>

#include "vrf/core/math.hpp"
#include "vrf/core/result.hpp"
#include "vrf/fg/resource_access.hpp"

namespace vrf
{
    class RenderDevice;
}

namespace vrf::fg
{
    class Texture
    {
    public:
        struct Desc
        {
            Extent2D             extent {};
            uint32_t             depth {0};
            VriFormat            format {VriFormat_Unknown};
            uint32_t             numMipLevels {1};
            uint32_t             layers {0};
            bool                 cubemap {false};
            VriTextureUsageFlags usage {VriTextureUsage_None};

            [[nodiscard]] bool operator==(const Desc&) const = default;
        };

        Texture() = default;
        ~Texture();

        Texture(const Texture&)            = delete;
        Texture& operator=(const Texture&) = delete;
        Texture(Texture&& other) noexcept;
        Texture& operator=(Texture&& other) noexcept;

        // Create and own a texture matching `desc` (transient-pool path).
        [[nodiscard]] static Expected<Texture> Create(RenderDevice&, const Desc&);

        // Wrap an externally-owned texture (swapchain / XR image). `state` is the
        // access/layout the texture is currently in.
        [[nodiscard]] static Texture Borrow(RenderDevice&, VriTexture*, const Desc&, const VriAccessLayoutStage& state);

        [[nodiscard]] VriTexture*  Handle() const noexcept { return m_texture; }
        [[nodiscard]] const Desc&  GetDesc() const noexcept { return m_desc; }
        [[nodiscard]] Extent2D     GetExtent() const noexcept { return m_desc.extent; }
        [[nodiscard]] VriFormat    GetFormat() const noexcept { return m_desc.format; }
        [[nodiscard]] uint32_t     GetNumLayers() const noexcept { return m_desc.layers; }
        [[nodiscard]] explicit operator bool() const noexcept { return m_texture != nullptr; }

        // Render-target view for one subresource (or the whole layer range when
        // layer/face are nullopt - multiview renders into all layers).
        [[nodiscard]] VriDescriptor* AttachmentView(ImageAspect,
                                                    std::optional<uint32_t> layer = std::nullopt,
                                                    std::optional<CubeFace> face  = std::nullopt);
        // Whole-resource sampled view of one aspect.
        [[nodiscard]] VriDescriptor* SampledView(ImageAspect = ImageAspect::Color);
        // Storage (UAV) view of one mip.
        [[nodiscard]] VriDescriptor* StorageView(uint32_t mipLevel = 0);

        // Last-known whole-resource state, updated by the barrier helpers in
        // RenderContext. Borrowed textures start in the state given to Borrow().
        VriAccessLayoutStage state {VriAccess_None, VriLayout_Undefined, VriPipelineStage_AllCommands};

    private:
        Texture(RenderDevice& device, VriTexture* texture, const Desc& desc, bool owned) noexcept;

        void DestroyViews() noexcept;
        void Reset() noexcept;

        [[nodiscard]] VriDescriptor* GetOrCreateView(uint64_t key, const VriTextureViewDesc&);

        RenderDevice* m_device  = nullptr;
        VriTexture*   m_texture = nullptr;
        Desc          m_desc {};
        bool          m_owned = false;

        std::unordered_map<uint64_t, VriDescriptor*> m_views;
    };
} // namespace vrf::fg
