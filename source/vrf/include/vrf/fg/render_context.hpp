/*
 * render_context.hpp - per-frame execution context passed as `ctx` through the
 * framegraph's execute callbacks.
 *
 * Collects the state that preRead/preWrite hooks declare (attachments into
 * framebufferInfo, shader bindings into resourceSet), and turns it into VRI
 * calls at pass execution: beginRendering() opens dynamic rendering from the
 * gathered attachments, bindDescriptorSets() allocates+writes descriptor sets
 * from the gathered bindings against a pipeline's layout.
 *
 * VRI needs the descriptor-set *layout* (register spaces + ranges) to allocate
 * sets, so pipelines travel with their layout description as a PassPipeline.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <glm/glm.hpp>
#include <vri/vri.h>

#include "vrf/core/math.hpp"
#include "vrf/fg/buffer.hpp"
#include "vrf/fg/resource_access.hpp"
#include "vrf/fg/texture.hpp"

namespace vrf
{
    class RenderDevice;
}

namespace vrf::fg
{
    // ---- pipeline + layout bundle ---------------------------------------

    // The layout description VRI needs both to create a pipeline layout and to
    // allocate/update descriptor sets against it later. Built once per pipeline
    // (usually from shader reflection) and shared by all its variants.
    struct PipelineLayoutInfo
    {
        VriPipelineLayout* handle {nullptr};

        struct Set
        {
            uint32_t                            registerSpace {0};
            std::vector<VriDescriptorRangeDesc> ranges;
        };
        std::vector<Set> sets;
    };

    struct PassPipeline
    {
        VriPipeline*                        pipeline {nullptr};
        std::shared_ptr<PipelineLayoutInfo> layout;
        bool                                compute {false};

        [[nodiscard]] explicit operator bool() const noexcept { return pipeline != nullptr; }
    };

    // ---- declarative resource bindings -----------------------------------

    namespace bindings
    {
        struct SeparateSampler
        {
            VriDescriptor* handle {nullptr};
        };
        // Sampled texture. Under VRI the sampler is always a separate binding;
        // this maps to a sampled view (name kept for source compatibility).
        struct CombinedImageSampler
        {
            Texture*    texture {nullptr};
            ImageAspect imageAspect {ImageAspect::Color};
        };
        struct SampledImage
        {
            Texture*    texture {nullptr};
            ImageAspect imageAspect {ImageAspect::Color};
        };
        struct StorageImage
        {
            Texture*                texture {nullptr};
            ImageAspect             imageAspect {ImageAspect::Color};
            std::optional<uint32_t> mipLevel;
        };
        struct UniformBuffer
        {
            Buffer* buffer {nullptr};
        };
        struct StorageBuffer
        {
            Buffer* buffer {nullptr};
        };
        // Pre-made view descriptor (acceleration structures, external views).
        struct RawDescriptor
        {
            VriDescriptor*    handle {nullptr};
            VriDescriptorType type {VriDescriptorType_Texture};
        };
    } // namespace bindings

    using ResourceBinding = std::variant<bindings::SeparateSampler,
                                         bindings::CombinedImageSampler,
                                         bindings::SampledImage,
                                         bindings::StorageImage,
                                         bindings::UniformBuffer,
                                         bindings::StorageBuffer,
                                         bindings::RawDescriptor>;

    using ResourceBindings = std::unordered_map<uint32_t, ResourceBinding>;   // binding -> resource
    using ResourceSet      = std::unordered_map<uint32_t, ResourceBindings>;  // set -> bindings
    using Samplers         = std::unordered_map<std::string, VriDescriptor*>; // name -> sampler

    // ---- attachments ------------------------------------------------------

    using ClearVariant = std::variant<glm::vec4, glm::ivec4, glm::uvec4, float, uint32_t>;

    struct AttachmentInfo
    {
        Texture*                    target {nullptr};
        std::optional<uint32_t>     layer;
        std::optional<CubeFace>     face;
        std::optional<ClearVariant> clearValue;
    };

    struct FramebufferInfo
    {
        Extent2D                      area {};
        uint32_t                      layers {1};
        uint32_t                      viewMask {0}; // multiview; 0 = single view
        std::optional<AttachmentInfo> depthAttachment;
        bool                          depthReadOnly {false};
        std::optional<AttachmentInfo> stencilAttachment;
        bool                          stencilReadOnly {false};
        std::vector<AttachmentInfo>   colorAttachments;
    };

    // ---- per-frame descriptor allocation ----------------------------------

    // Grows-on-demand pool set; Reset() recycles everything for the next frame.
    class DescriptorAllocator
    {
    public:
        DescriptorAllocator() = default;
        explicit DescriptorAllocator(RenderDevice&);
        ~DescriptorAllocator();

        DescriptorAllocator(const DescriptorAllocator&)            = delete;
        DescriptorAllocator& operator=(const DescriptorAllocator&) = delete;
        DescriptorAllocator(DescriptorAllocator&&) noexcept;
        DescriptorAllocator& operator=(DescriptorAllocator&&) noexcept;

        [[nodiscard]] VriDescriptorSet* Allocate(const PipelineLayoutInfo&, uint32_t setIndex);

        void Reset();

    private:
        void Release() noexcept;

        RenderDevice*                   m_device = nullptr;
        std::vector<VriDescriptorPool*> m_pools;
        std::size_t                     m_active = 0;
    };

    // ---- the context -------------------------------------------------------

    class RenderContext
    {
    public:
        RenderContext(RenderDevice&        device,
                      VriCommandBuffer*    cmd,
                      Samplers&            samplers,
                      DescriptorAllocator& descriptors);
        RenderContext(const RenderContext&) = delete;
        virtual ~RenderContext()            = default;

        RenderDevice&                  device;
        VriCommandBuffer*              cmd {nullptr};
        std::optional<FramebufferInfo> framebufferInfo;
        ResourceSet                    resourceSet;
        Samplers&                      samplers;
        DescriptorAllocator&           descriptors;

        // -- explicit barrier helpers (used by the preRead/preWrite hooks) --
        void TransitionTexture(Texture&, const VriAccessLayoutStage& after);
        void TransitionBuffer(Buffer&, const VriAccessStage& after);

        // -- rendering --------------------------------------------------------
        // Open dynamic rendering from the attachments preWrite gathered into
        // framebufferInfo. Sets full-target viewport/scissor.
        void BeginRendering();
        void EndRendering();

        // Bind pipeline + layout, then allocate/write/bind descriptor sets from
        // resourceSet. Clears the consumed bindings.
        void BindPipeline(const PassPipeline&);

        // Allocate/write/bind ONE register space from resourceSet, leaving the
        // declarations intact - for passes that rebind a material set per draw
        // while a shared set (camera) stays bound. Pipeline+layout must already
        // be set on the command buffer.
        void BindSingleDescriptorSet(const PassPipeline&, uint32_t registerSpace);

        // Fullscreen triangle helper: BeginRendering + BindPipeline + draw(3) +
        // EndRendering.
        void RenderFullScreenPostProcess(const PassPipeline&);

        // Replace/insert a sampler binding at (set, binding).
        void SetSampler(uint32_t set, uint32_t binding, VriDescriptor* sampler);

    private:
        void BindDescriptorSets(const PassPipeline&);

        // Reused across BindSingleDescriptorSet calls so per-draw descriptor binding does not
        // heap-allocate a fresh view list every draw (recording is single-threaded).
        std::vector<const VriDescriptor*> m_bindScratch;
    };
} // namespace vrf::fg
