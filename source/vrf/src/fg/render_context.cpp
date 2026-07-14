#include "vrf/fg/render_context.hpp"

#include <cassert>

#include "vrf/core/log.hpp"
#include "vrf/gpu/render_device.hpp"

namespace vrf::fg
{
    namespace
    {
        [[nodiscard]] VriClearValue convert(const ClearVariant& in, const bool depthStencil)
        {
            VriClearValue out {};
            if (const auto* v = std::get_if<glm::vec4>(&in))
            {
                out.color.f32[0] = v->x;
                out.color.f32[1] = v->y;
                out.color.f32[2] = v->z;
                out.color.f32[3] = v->w;
            }
            else if (const auto* iv = std::get_if<glm::ivec4>(&in))
            {
                out.color.i32[0] = iv->x;
                out.color.i32[1] = iv->y;
                out.color.i32[2] = iv->z;
                out.color.i32[3] = iv->w;
            }
            else if (const auto* uv = std::get_if<glm::uvec4>(&in))
            {
                out.color.u32[0] = uv->x;
                out.color.u32[1] = uv->y;
                out.color.u32[2] = uv->z;
                out.color.u32[3] = uv->w;
            }
            else if (const auto* f = std::get_if<float>(&in))
            {
                if (depthStencil)
                {
                    out.depthStencil.depth = *f;
                }
                else
                {
                    out.color.f32[0] = out.color.f32[1] = out.color.f32[2] = out.color.f32[3] = *f;
                }
            }
            else if (const auto* u = std::get_if<uint32_t>(&in))
            {
                if (depthStencil)
                {
                    out.depthStencil.stencil = *u;
                }
                else
                {
                    out.color.u32[0] = out.color.u32[1] = out.color.u32[2] = out.color.u32[3] = *u;
                }
            }
            return out;
        }

        [[nodiscard]] VriAttachmentDesc makeAttachment(const AttachmentInfo& in, const ImageAspect aspect)
        {
            return VriAttachmentDesc {
                .view    = in.target->AttachmentView(aspect, in.layer, in.face),
                .loadOp  = in.clearValue ? VriAttachmentLoadOp_Clear : VriAttachmentLoadOp_Load,
                .storeOp = VriAttachmentStoreOp_Store,
                .clearValue =
                    in.clearValue ? convert(*in.clearValue, aspect != ImageAspect::Color) : VriClearValue {},
                .resolveView = nullptr,
            };
        }

        [[nodiscard]] VriImageAspectFlags formatAspect(const VriFormat format)
        {
            switch (format)
            {
                case VriFormat_D16_UNORM:
                case VriFormat_D32_SFLOAT:
                    return VriImageAspect_Depth;
                case VriFormat_S8_UINT:
                    return VriImageAspect_Stencil;
                case VriFormat_D24_UNORM_S8_UINT:
                case VriFormat_D32_SFLOAT_S8_UINT:
                    return VriImageAspect_Depth | VriImageAspect_Stencil;
                default:
                    return VriImageAspect_Color;
            }
        }
    } // namespace

    // ---- DescriptorAllocator ------------------------------------------------

    DescriptorAllocator::DescriptorAllocator(RenderDevice& device) : m_device {&device} {}

    DescriptorAllocator::~DescriptorAllocator() { Release(); }

    DescriptorAllocator::DescriptorAllocator(DescriptorAllocator&& other) noexcept :
        m_device {other.m_device}, m_pools {std::move(other.m_pools)}, m_active {other.m_active}
    {
        other.m_pools.clear();
    }

    DescriptorAllocator& DescriptorAllocator::operator=(DescriptorAllocator&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_device = other.m_device;
            m_pools  = std::move(other.m_pools);
            m_active = other.m_active;
            other.m_pools.clear();
        }
        return *this;
    }

    VriDescriptorSet* DescriptorAllocator::Allocate(const PipelineLayoutInfo& layout, const uint32_t setIndex)
    {
        assert(m_device && layout.handle);

        auto tryAllocate = [&](VriDescriptorPool* pool) -> VriDescriptorSet* {
            VriDescriptorSet* set = nullptr;
            const auto r = m_device->Core().AllocateDescriptorSets(pool, layout.handle, setIndex, &set, 1);
            return Succeeded(r) ? set : nullptr;
        };

        if (m_active < m_pools.size())
        {
            if (auto* set = tryAllocate(m_pools[m_active]))
            {
                return set;
            }
            ++m_active; // current pool exhausted
        }

        if (m_active >= m_pools.size())
        {
            const VriDescriptorPoolDesc poolDesc {
                .descriptorSetMaxNum         = 512,
                .samplerMaxNum               = 512,
                .textureMaxNum               = 2048,
                .storageTextureMaxNum        = 1024,
                .constantBufferMaxNum        = 512,
                .structuredBufferMaxNum      = 512,
                .storageBufferMaxNum         = 1024,
                .accelerationStructureMaxNum = m_device->Desc()->hasRayTracing ? 64u : 0u,
            };
            VriDescriptorPool* pool = nullptr;
            if (const auto r = m_device->Core().CreateDescriptorPool(m_device->Handle(), &poolDesc, &pool);
                !Succeeded(r))
            {
                LogError("fg::DescriptorAllocator: CreateDescriptorPool failed ({})", ToString(r));
                return nullptr;
            }
            m_pools.push_back(pool);
        }

        if (auto* set = tryAllocate(m_pools[m_active]))
        {
            return set;
        }
        LogError("fg::DescriptorAllocator: allocation failed on a fresh pool");
        return nullptr;
    }

    void DescriptorAllocator::Reset()
    {
        for (auto* pool : m_pools)
        {
            m_device->Core().ResetDescriptorPool(pool);
        }
        m_active = 0;
    }

    void DescriptorAllocator::Release() noexcept
    {
        if (m_device)
        {
            for (auto* pool : m_pools)
            {
                m_device->Core().DestroyDescriptorPool(pool);
            }
        }
        m_pools.clear();
    }

    // ---- RenderContext --------------------------------------------------------

    RenderContext::RenderContext(RenderDevice&        device,
                                 VriCommandBuffer*    cmd,
                                 Samplers&            samplers,
                                 DescriptorAllocator& descriptors) :
        device {device}, cmd {cmd}, samplers {samplers}, descriptors {descriptors}
    {}

    void RenderContext::TransitionTexture(Texture& texture, const VriAccessLayoutStage& after)
    {
        if (texture.state.access == after.access && texture.state.layout == after.layout &&
            texture.state.stages == after.stages)
        {
            // Same-state storage access still needs an execution barrier between
            // passes (WAW/RAW on the same layout).
            if (after.layout != VriLayout_ShaderResourceStorage && after.layout != VriLayout_General)
            {
                return;
            }
        }

        const VriTextureBarrierDesc barrier {
            .texture = texture.Handle(),
            .before  = texture.state,
            .after   = after,
            .aspect  = formatAspect(texture.GetFormat()),
        };
        const VriBarrierGroupDesc group {.textures = &barrier, .textureNum = 1};
        device.Core().CmdBarrier(cmd, &group);
        texture.state = after;
    }

    void RenderContext::TransitionBuffer(Buffer& buffer, const VriAccessStage& after)
    {
        const VriBufferBarrierDesc barrier {
            .buffer = buffer.Handle(),
            .before = buffer.state,
            .after  = after,
        };
        const VriBarrierGroupDesc group {.buffers = &barrier, .bufferNum = 1};
        device.Core().CmdBarrier(cmd, &group);
        buffer.state = after;
    }

    void RenderContext::BeginRendering()
    {
        assert(framebufferInfo && "no attachments declared for this pass");
        auto& fb = *framebufferInfo;

        std::vector<VriAttachmentDesc> colors;
        colors.reserve(fb.colorAttachments.size());
        for (const auto& attachment : fb.colorAttachments)
        {
            colors.push_back(makeAttachment(attachment, ImageAspect::Color));
        }

        std::optional<VriAttachmentDesc> depth;
        if (fb.depthAttachment)
        {
            depth = makeAttachment(*fb.depthAttachment, ImageAspect::Depth);
        }
        std::optional<VriAttachmentDesc> stencil;
        if (fb.stencilAttachment)
        {
            stencil = makeAttachment(*fb.stencilAttachment, ImageAspect::Stencil);
        }

        const VriAttachmentsDesc desc {
            .colors     = colors.data(),
            .colorNum   = static_cast<uint32_t>(colors.size()),
            .depth      = depth ? &*depth : nullptr,
            .stencil    = stencil ? &*stencil : nullptr,
            .renderArea = {0, 0, fb.area.width, fb.area.height},
            .layerNum   = fb.layers,
            .viewMask   = fb.viewMask,
        };
        device.Core().CmdBeginRendering(cmd, &desc);

        const VriViewport viewport {0.0f, 0.0f, static_cast<float>(fb.area.width),
                                    static_cast<float>(fb.area.height), 0.0f, 1.0f};
        device.Core().CmdSetViewports(cmd, &viewport, 1);
        const VriRect scissor {0, 0, fb.area.width, fb.area.height};
        device.Core().CmdSetScissors(cmd, &scissor, 1);
    }

    void RenderContext::EndRendering()
    {
        device.Core().CmdEndRendering(cmd);
        // Attachment declarations are per-pass; the next pass's preWrite hooks
        // start from a clean slate.
        framebufferInfo.reset();
    }

    void RenderContext::BindPipeline(const PassPipeline& pipeline)
    {
        assert(pipeline);
        device.Core().CmdSetPipelineLayout(cmd, pipeline.layout->handle);
        device.Core().CmdSetPipeline(cmd, pipeline.pipeline);
        BindDescriptorSets(pipeline);
    }

    void RenderContext::RenderFullScreenPostProcess(const PassPipeline& pipeline)
    {
        BeginRendering();
        BindPipeline(pipeline);
        const VriDrawDesc draw {.vertexNum = 3, .instanceNum = 1};
        device.Core().CmdDraw(cmd, &draw);
        EndRendering();
    }

    void RenderContext::SetSampler(const uint32_t set, const uint32_t binding, VriDescriptor* sampler)
    {
        resourceSet[set][binding] = bindings::SeparateSampler {sampler};
    }

    void RenderContext::BindDescriptorSets(const PassPipeline& pipeline)
    {
        for (const auto& setLayout : pipeline.layout->sets)
        {
            BindSingleDescriptorSet(pipeline, setLayout.registerSpace);
        }
        resourceSet.clear();
    }

    void RenderContext::BindSingleDescriptorSet(const PassPipeline& pipeline, const uint32_t registerSpace)
    {
        const auto& layout = *pipeline.layout;

        const auto layoutIt = std::find_if(layout.sets.begin(), layout.sets.end(), [&](const auto& set) {
            return set.registerSpace == registerSpace;
        });
        if (layoutIt == layout.sets.end())
        {
            return;
        }
        const auto& setLayout = *layoutIt;

        {
            const auto setIt = resourceSet.find(setLayout.registerSpace);
            if (setIt == resourceSet.end())
            {
                return;
            }
            const auto& boundResources = setIt->second;

            auto* set = descriptors.Allocate(layout, setLayout.registerSpace);
            if (!set)
            {
                return;
            }

            // One update per range; range order in the layout is authoritative.
            for (uint32_t rangeIndex = 0; rangeIndex < setLayout.ranges.size(); ++rangeIndex)
            {
                const auto& range = setLayout.ranges[rangeIndex];

                std::vector<const VriDescriptor*> views;
                views.reserve(range.descriptorNum);
                for (uint32_t i = 0; i < range.descriptorNum; ++i)
                {
                    const auto bindingIt = boundResources.find(range.baseRegister + i);
                    if (bindingIt == boundResources.end())
                    {
                        break; // partially-bound tail
                    }

                    const VriDescriptor* view = std::visit(
                        [&](const auto& b) -> const VriDescriptor* {
                            using T = std::decay_t<decltype(b)>;
                            if constexpr (std::is_same_v<T, bindings::SeparateSampler>)
                            {
                                return b.handle;
                            }
                            else if constexpr (std::is_same_v<T, bindings::CombinedImageSampler> ||
                                               std::is_same_v<T, bindings::SampledImage>)
                            {
                                return b.texture->SampledView(b.imageAspect);
                            }
                            else if constexpr (std::is_same_v<T, bindings::StorageImage>)
                            {
                                return b.texture->StorageView(b.mipLevel.value_or(0));
                            }
                            else if constexpr (std::is_same_v<T, bindings::UniformBuffer>)
                            {
                                return b.buffer->View(VriDescriptorType_ConstantBuffer);
                            }
                            else if constexpr (std::is_same_v<T, bindings::StorageBuffer>)
                            {
                                return b.buffer->View(range.descriptorType ==
                                                              VriDescriptorType_StructuredBuffer ?
                                                          VriDescriptorType_StructuredBuffer :
                                                          VriDescriptorType_StorageBuffer);
                            }
                            else if constexpr (std::is_same_v<T, bindings::RawDescriptor>)
                            {
                                return b.handle;
                            }
                            return nullptr;
                        },
                        bindingIt->second);

                    if (!view)
                    {
                        break;
                    }
                    views.push_back(view);
                }

                if (views.empty())
                {
                    continue;
                }
                const VriDescriptorRangeUpdateDesc update {
                    .descriptors    = views.data(),
                    .descriptorNum  = static_cast<uint32_t>(views.size()),
                    .baseDescriptor = 0,
                };
                device.Core().UpdateDescriptorRanges(set, rangeIndex, 1, &update);
            }

            device.Core().CmdSetDescriptorSet(cmd, setLayout.registerSpace, set);
        }
    }
} // namespace vrf::fg
