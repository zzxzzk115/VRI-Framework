#include "vrf/fg/framegraph_texture.hpp"

#include <cassert>
#include <format>

#include "vrf/fg/render_context.hpp"
#include "vrf/fg/transient_resources.hpp"

namespace vrf::fg
{
    namespace
    {
        void declareAttachment(RenderContext& rc, Texture* texture, const uint32_t bits, const bool readOnly)
        {
            auto& fb = rc.framebufferInfo;
            if (!fb)
            {
                fb.emplace().area = texture->GetExtent();
            }

            const auto attachment = decodeAttachment(bits);
            const AttachmentInfo info {
                .target     = texture,
                .layer      = attachment.layer,
                .face       = attachment.face,
                .clearValue = std::nullopt,
            };

            switch (attachment.imageAspect)
            {
                case ImageAspect::Depth:
                    fb->depthAttachment = info;
                    fb->depthReadOnly   = readOnly;
                    break;
                case ImageAspect::Stencil:
                    fb->stencilAttachment = info;
                    fb->stencilReadOnly   = readOnly;
                    break;
                case ImageAspect::Color: {
                    assert(!readOnly);
                    auto& v = fb->colorAttachments;
                    if (v.size() <= attachment.index)
                    {
                        v.resize(attachment.index + 1);
                    }
                    v[attachment.index] = info;
                    break;
                }
                default:
                    assert(false);
            }
        }

        // Convert a write-attachment clear declaration (decoded lazily so the
        // clear value reaches the AttachmentInfo).
        [[nodiscard]] std::optional<ClearVariant> convertClear(const std::optional<ClearValue> clear)
        {
            if (!clear)
            {
                return std::nullopt;
            }
            switch (*clear)
            {
                case ClearValue::Zero:
                    return 0.0f;
                case ClearValue::One:
                    return 1.0f;
                case ClearValue::OpaqueBlack:
                    return glm::vec4 {0.0f, 0.0f, 0.0f, 1.0f};
                case ClearValue::OpaqueWhite:
                    return glm::vec4 {1.0f};
                case ClearValue::TransparentBlack:
                    return glm::vec4 {0.0f};
                case ClearValue::TransparentWhite:
                    return glm::vec4 {1.0f, 1.0f, 1.0f, 0.0f};
                case ClearValue::FakeSky:
                    return glm::vec4 {0.529f, 0.808f, 0.922f, 1.0f};
                case ClearValue::UIntMax:
                    return glm::uvec4 {~0u};
            }
            return std::nullopt;
        }
    } // namespace

    void FrameGraphTexture::create(const Desc& desc, void* allocator)
    {
        texture = static_cast<TransientResources*>(allocator)->acquireTexture(desc);
    }

    void FrameGraphTexture::destroy(const Desc& desc, void* allocator)
    {
        static_cast<TransientResources*>(allocator)->releaseTexture(desc, texture);
        texture = nullptr;
    }

    void FrameGraphTexture::preRead(const Desc&, const uint32_t bits, void* ctx) const
    {
        auto& rc = *static_cast<RenderContext*>(ctx);

        if (holdsAttachment(bits))
        {
            // Read-only depth/stencil attachment.
            declareAttachment(rc, texture, bits, true);
            rc.TransitionTexture(*texture,
                                 {VriAccess_DepthStencilAttachmentRead,
                                  VriLayout_DepthStencilReadOnly,
                                  VriPipelineStage_EarlyFragmentTests | VriPipelineStage_LateFragmentTests |
                                      VriPipelineStage_FragmentShader});
            return;
        }

        const auto read                      = decodeTextureRead(bits);
        const auto [location, pipelineStage] = read.binding;

        if (static_cast<bool>(pipelineStage & PipelineStage::Transfer))
        {
            rc.TransitionTexture(*texture, {VriAccess_CopySourceRead, VriLayout_CopySource, VriPipelineStage_Transfer});
            return;
        }

        const auto stages = convert(pipelineStage);
        switch (read.type)
        {
            case TextureRead::Type::CombinedImageSampler:
            case TextureRead::Type::SampledImage:
                rc.resourceSet[location.set][location.binding] =
                    bindings::SampledImage {texture, read.imageAspect};
                rc.TransitionTexture(*texture, {VriAccess_ShaderResourceRead, VriLayout_ShaderResource, stages});
                break;
            case TextureRead::Type::StorageImage:
                rc.resourceSet[location.set][location.binding] =
                    bindings::StorageImage {texture, read.imageAspect, 0};
                rc.TransitionTexture(
                    *texture, {VriAccess_ShaderResourceStorageRead, VriLayout_ShaderResourceStorage, stages});
                break;
        }
    }

    void FrameGraphTexture::preWrite(const Desc&, const uint32_t bits, void* ctx) const
    {
        auto& rc = *static_cast<RenderContext*>(ctx);

        if (holdsAttachment(bits))
        {
            const auto attachment = decodeAttachment(bits);
            declareAttachment(rc, texture, bits, false);

            // Attach the clear value onto the declared attachment.
            auto& fb = *rc.framebufferInfo;
            switch (attachment.imageAspect)
            {
                case ImageAspect::Depth:
                    fb.depthAttachment->clearValue = convertClear(attachment.clearValue);
                    rc.TransitionTexture(*texture,
                                         {VriAccess_DepthStencilAttachmentRead | VriAccess_DepthStencilAttachmentWrite,
                                          VriLayout_DepthStencilAttachment,
                                          VriPipelineStage_EarlyFragmentTests | VriPipelineStage_LateFragmentTests});
                    break;
                case ImageAspect::Stencil:
                    fb.stencilAttachment->clearValue = convertClear(attachment.clearValue);
                    rc.TransitionTexture(*texture,
                                         {VriAccess_DepthStencilAttachmentRead | VriAccess_DepthStencilAttachmentWrite,
                                          VriLayout_DepthStencilAttachment,
                                          VriPipelineStage_EarlyFragmentTests | VriPipelineStage_LateFragmentTests});
                    break;
                case ImageAspect::Color:
                    fb.colorAttachments[attachment.index].clearValue = convertClear(attachment.clearValue);
                    rc.TransitionTexture(*texture,
                                         {VriAccess_ColorAttachmentWrite,
                                          VriLayout_ColorAttachment,
                                          VriPipelineStage_ColorAttachmentOutput});
                    break;
                default:
                    assert(false);
            }
            return;
        }

        const auto write                     = decodeImageWrite(bits);
        const auto [location, pipelineStage] = write.binding;
        assert(write.imageAspect != ImageAspect::None);

        if (static_cast<bool>(pipelineStage & PipelineStage::Transfer))
        {
            rc.TransitionTexture(*texture,
                                 {VriAccess_CopyDestinationWrite, VriLayout_CopyDestination, VriPipelineStage_Transfer});
            return;
        }

        rc.resourceSet[location.set][location.binding] = bindings::StorageImage {texture, write.imageAspect, 0};
        rc.TransitionTexture(*texture,
                             {VriAccess_ShaderResourceStorageRead | VriAccess_ShaderResourceStorageWrite,
                              VriLayout_ShaderResourceStorage,
                              convert(pipelineStage)});
    }

    std::string FrameGraphTexture::toString(const Desc& desc)
    {
        return std::format("{}x{} (format {})", desc.extent.width, desc.extent.height,
                           static_cast<int>(desc.format));
    }
} // namespace vrf::fg
