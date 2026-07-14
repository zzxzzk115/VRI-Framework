// vrf framegraph_deferred - validation example for the vrf::fg framegraph
// module: a 3-pass deferred pipeline (gbuffer MRT -> lighting -> tonemap to the
// swapchain), with every inter-pass barrier, transient allocation, and
// descriptor set produced by the framegraph hooks rather than by hand.
//
// Prints transient-pool stats on exit. Run with Vulkan validation +
// synchronization validation to gate the module (see the A1 checkpoint).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <fg/Blackboard.hpp>
#include <fg/FrameGraph.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vrf/core/log.hpp>
#include <vrf/fg/framegraph_import.hpp>
#include <vrf/fg/framegraph_texture.hpp>
#include <vrf/fg/render_context.hpp>
#include <vrf/fg/transient_resources.hpp>
#include <vrf/gpu/builders/graphics_pipeline_builder.hpp>
#include <vrf/gpu/builders/pipeline_layout_builder.hpp>
#include <vrf/gpu/frame_stream.hpp>
#include <vrf/gpu/render_device.hpp>
#include <vrf/gpu/shader_library.hpp>
#include <vrf/gpu/swapchain.hpp>
#include <vrf/platform/window.hpp>

namespace
{
    const unsigned char g_deferredVshlib[] = {
#include "deferred.vshlib.h"
    };

    struct PushConstants
    {
        glm::mat4 mvp;
        glm::mat4 model;
    };

    // Build a graphics pipeline + its layout-info bundle from cooked shaders.
    struct PipelineSetup
    {
        vrf::fg::PassPipeline pipeline;
    };

    [[nodiscard]] vrf::Expected<vrf::fg::PassPipeline>
    makePipeline(vrf::RenderDevice&                                   device,
                 vrf::ShaderLibrary&                                  shaders,
                 const char*                                          shaderId,
                 const std::vector<vrf::fg::PipelineLayoutInfo::Set>& sets,
                 const std::vector<VriPushConstantDesc>&              pushConstants,
                 const std::vector<VriFormat>&                        colorFormats,
                 VriFormat                                            depthFormat,
                 VriCullMode                                          cullMode)
    {
        auto vs = shaders.Resolve(shaderId, vrf::ShaderStage::Vertex, {});
        if (!vs)
        {
            return std::unexpected(vs.error());
        }
        auto fs = shaders.Resolve(shaderId, vrf::ShaderStage::Fragment, {});
        if (!fs)
        {
            return std::unexpected(fs.error());
        }

        vrf::PipelineLayoutBuilder layoutBuilder;
        layoutBuilder.SetShaderStages(VriShaderStage_AllGraphics);
        for (const auto& set : sets)
        {
            layoutBuilder.AddDescriptorSet(set.registerSpace, set.ranges);
        }
        for (const auto& pc : pushConstants)
        {
            layoutBuilder.AddPushConstant(pc.baseRegister, pc.size, pc.shaderStages);
        }
        auto layout = layoutBuilder.Build(device);
        if (!layout)
        {
            return std::unexpected(layout.error());
        }

        vrf::GraphicsPipelineBuilder pipelineBuilder;
        pipelineBuilder.SetPipelineLayout(*layout)
            .AddShader(VriShaderStage_Vertex, vs->spirv, vs->spirvSize, vs->entryPoint.c_str())
            .AddShader(VriShaderStage_Fragment, fs->spirv, fs->spirvSize, fs->entryPoint.c_str())
            .SetTopology(VriPrimitiveTopology_TriangleList)
            .SetCullMode(cullMode);
        for (const auto format : colorFormats)
        {
            pipelineBuilder.AddColorAttachment(format);
        }
        if (depthFormat != VriFormat_Unknown)
        {
            pipelineBuilder.SetDepthStencilFormat(depthFormat).SetDepthTest(true, true);
        }

        auto pipeline = pipelineBuilder.Build(device);
        if (!pipeline)
        {
            return std::unexpected(pipeline.error());
        }

        auto layoutInfo    = std::make_shared<vrf::fg::PipelineLayoutInfo>();
        layoutInfo->handle = *layout;
        layoutInfo->sets   = sets;
        return vrf::fg::PassPipeline {*pipeline, std::move(layoutInfo), false};
    }
} // namespace

int main()
{
    constexpr vrf::Extent2D kExtent {1280, 720};

    // ---- window / device / swapchain / frame -----------------------------
    vrf::WindowDesc windowDesc;
    windowDesc.title  = "vrf framegraph_deferred";
    windowDesc.extent = kExtent;
    auto windowResult = vrf::Window::Create(windowDesc);
    if (!windowResult)
    {
        std::fprintf(stderr, "[framegraph-deferred] window: %s\n", windowResult.error().message.c_str());
        return 1;
    }
    auto& window = *windowResult.value();

    vrf::RenderDeviceDesc deviceDesc;
    deviceDesc.validation = true;
    auto deviceResult     = vrf::RenderDevice::Create(deviceDesc);
    if (!deviceResult)
    {
        std::fprintf(stderr, "[framegraph-deferred] device: %s\n", deviceResult.error().message.c_str());
        return 1;
    }
    vrf::RenderDevice& device = *deviceResult;

    vrf::SwapchainDesc swapDesc;
    swapDesc.window = window.Handle();
    swapDesc.extent = kExtent;
    auto swapResult = vrf::Swapchain::Create(device, swapDesc);
    if (!swapResult)
    {
        std::fprintf(stderr, "[framegraph-deferred] swapchain: %s\n", swapResult.error().message.c_str());
        return 1;
    }
    vrf::Swapchain& swapchain = *swapResult;

    constexpr uint32_t kFramesInFlight = 2;

    auto frameResult = vrf::FrameStream::Create(device, kFramesInFlight);
    if (!frameResult)
    {
        std::fprintf(stderr, "[framegraph-deferred] frame stream: %s\n", frameResult.error().message.c_str());
        return 1;
    }
    vrf::FrameStream frames = std::move(*frameResult);

    // ---- shaders ----------------------------------------------------------
    auto libResult = vrf::ShaderLibrary::LoadFromMemory(g_deferredVshlib, sizeof(g_deferredVshlib));
    if (!libResult)
    {
        std::fprintf(stderr, "[framegraph-deferred] shaders: %s\n", libResult.error().message.c_str());
        return 1;
    }
    vrf::ShaderLibrary shaders = std::move(*libResult);

    // ---- framegraph services ----------------------------------------------
    vrf::fg::TransientResources transientResources {device, kFramesInFlight};
    // One descriptor allocator per frame slot: sets from frame N-1 are still
    // consumed by the GPU while the CPU records frame N.
    std::vector<vrf::fg::DescriptorAllocator> descriptorAllocators;
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        descriptorAllocators.emplace_back(device);
    }
    vrf::fg::Samplers samplers;

    {
        VriSamplerDesc samplerDesc {};
        samplerDesc.magFilter    = VriFilter_Linear;
        samplerDesc.minFilter    = VriFilter_Linear;
        samplerDesc.mipmapMode   = VriMipmapMode_Nearest;
        samplerDesc.addressModeU = VriAddressMode_ClampToEdge;
        samplerDesc.addressModeV = VriAddressMode_ClampToEdge;
        samplerDesc.addressModeW = VriAddressMode_ClampToEdge;
        VriDescriptor* sampler   = nullptr;
        if (!vrf::Succeeded(device.Core().CreateSampler(device.Handle(), &samplerDesc, &sampler)))
        {
            std::fprintf(stderr, "[framegraph-deferred] sampler creation failed\n");
            return 1;
        }
        samplers["linearClamp"] = sampler;
    }

    // Persistent fg wrappers around the swapchain images (they carry layout state
    // across frames: Undefined on first use, Present after each frame).
    constexpr VriFormat kDepthFormat  = VriFormat_D32_SFLOAT;
    constexpr VriFormat kHdrFormat    = VriFormat_RGBA16_SFLOAT;
    constexpr VriFormat kAlbedoFormat = VriFormat_RGBA8_UNORM;

    std::vector<vrf::fg::Texture> backbuffers;
    auto                          rebuildBackbuffers = [&] {
        backbuffers.clear();
        for (uint32_t i = 0; i < swapchain.TextureCount(); ++i)
        {
            backbuffers.push_back(vrf::fg::Texture::Borrow(
                device,
                swapchain.Texture(i),
                {.extent = swapchain.Extent(), .format = swapchain.Format(), .usage = VriTextureUsage_ColorAttachment},
                {VriAccess_None, VriLayout_Undefined, VriPipelineStage_None}));
        }
    };
    rebuildBackbuffers();

    // ---- pipelines ----------------------------------------------------------
    using Set = vrf::fg::PipelineLayoutInfo::Set;

    auto gbufferPipeline  = makePipeline(device,
                                        shaders,
                                        "gbuffer",
                                         {},
                                         {{0, sizeof(PushConstants), VriShaderStage_Vertex}},
                                         {kAlbedoFormat, kHdrFormat},
                                        kDepthFormat,
                                        VriCullMode_Back);
    auto lightingPipeline = makePipeline(device,
                                         shaders,
                                         "lighting",
                                         {Set {0,
                                               {
                                                   {.baseRegister   = 0,
                                                    .descriptorNum  = 1,
                                                    .descriptorType = VriDescriptorType_Texture,
                                                    .shaderStages   = VriShaderStage_Fragment},
                                                   {.baseRegister   = 1,
                                                    .descriptorNum  = 1,
                                                    .descriptorType = VriDescriptorType_Texture,
                                                    .shaderStages   = VriShaderStage_Fragment},
                                                   {.baseRegister   = 2,
                                                    .descriptorNum  = 1,
                                                    .descriptorType = VriDescriptorType_Sampler,
                                                    .shaderStages   = VriShaderStage_Fragment},
                                               }}},
                                         {},
                                         {kHdrFormat},
                                         VriFormat_Unknown,
                                         VriCullMode_None);
    auto tonemapPipeline  = makePipeline(device,
                                        shaders,
                                        "tonemap",
                                         {Set {0,
                                               {
                                                  {.baseRegister   = 0,
                                                    .descriptorNum  = 1,
                                                    .descriptorType = VriDescriptorType_Texture,
                                                    .shaderStages   = VriShaderStage_Fragment},
                                                  {.baseRegister   = 1,
                                                    .descriptorNum  = 1,
                                                    .descriptorType = VriDescriptorType_Sampler,
                                                    .shaderStages   = VriShaderStage_Fragment},
                                              }}},
                                         {},
                                         {swapchain.Format()},
                                        VriFormat_Unknown,
                                        VriCullMode_None);
    if (!gbufferPipeline || !lightingPipeline || !tonemapPipeline)
    {
        const auto& err = !gbufferPipeline  ? gbufferPipeline.error() :
                          !lightingPipeline ? lightingPipeline.error() :
                                              tonemapPipeline.error();
        std::fprintf(stderr, "[framegraph-deferred] pipeline: %s\n", err.message.c_str());
        return 1;
    }

    // ---- frame loop -----------------------------------------------------------
    const auto start      = std::chrono::steady_clock::now();
    uint64_t   frameCount = 0;
    // Exit automatically after a few seconds when driven by CI/verification.
    const bool autoExit      = std::getenv("VRF_EXAMPLE_AUTO_EXIT") != nullptr;
    const auto autoExitAfter = std::chrono::seconds(5);

    while (!window.ShouldClose())
    {
        window.PollEvents();

        if (window.Extent() != swapchain.Extent() && window.Extent().width > 0 && window.Extent().height > 0)
        {
            device.WaitIdle();
            swapchain.Resize(window.Extent());
            rebuildBackbuffers();
        }

        const auto acquired = swapchain.Acquire();
        if (acquired.outOfDate)
        {
            device.WaitIdle();
            swapchain.Resize(window.Extent());
            rebuildBackbuffers();
            continue;
        }

        // Begin() waited for this slot's previous frame, so its descriptor pools
        // are safe to recycle now.
        VriCommandBuffer* cmd           = frames.Begin();
        auto&             slotAllocator = descriptorAllocators[frames.FrameIndex()];

        transientResources.update();
        slotAllocator.Reset();
        vrf::fg::RenderContext renderContext {device, cmd, samplers, slotAllocator};

        const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();

        const auto extent = swapchain.Extent();
        const auto aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);

        PushConstants pushConstants;
        pushConstants.model =
            glm::rotate(glm::mat4 {1.0f}, seconds * 0.8f, glm::normalize(glm::vec3 {0.3f, 1.0f, 0.2f}));
        const glm::mat4 view =
            glm::lookAt(glm::vec3 {0.0f, 0.6f, 2.2f}, glm::vec3 {0.0f}, glm::vec3 {0.0f, 1.0f, 0.0f});
        const glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 10.0f);
        pushConstants.mvp    = proj * view * pushConstants.model;

        // ---- build & run this frame's graph --------------------------------
        FrameGraph           graph;
        FrameGraphBlackboard blackboard;

        auto backbufferId = vrf::fg::importTexture(graph, "Backbuffer", &backbuffers[acquired.index]);

        struct GBufferData
        {
            FrameGraphResource albedo;
            FrameGraphResource normal;
            FrameGraphResource depth;
        };
        const auto& gbuffer = graph.addCallbackPass<GBufferData>(
            "GBuffer",
            [&](FrameGraph::Builder& builder, GBufferData& data) {
                data.albedo = builder.create<vrf::fg::FrameGraphTexture>(
                    "Albedo",
                    {.extent = extent,
                     .format = kAlbedoFormat,
                     .usage  = VriTextureUsage_ColorAttachment | VriTextureUsage_ShaderResource});
                data.normal = builder.create<vrf::fg::FrameGraphTexture>(
                    "Normal",
                    {.extent = extent,
                     .format = kHdrFormat,
                     .usage  = VriTextureUsage_ColorAttachment | VriTextureUsage_ShaderResource});
                data.depth = builder.create<vrf::fg::FrameGraphTexture>(
                    "Depth",
                    {.extent = extent, .format = kDepthFormat, .usage = VriTextureUsage_DepthStencilAttachment});

                data.albedo = builder.write(data.albedo,
                                            vrf::fg::Attachment {.index       = 0,
                                                                 .imageAspect = vrf::fg::ImageAspect::Color,
                                                                 .clearValue  = vrf::fg::ClearValue::TransparentBlack});
                data.normal = builder.write(data.normal,
                                            vrf::fg::Attachment {.index       = 1,
                                                                 .imageAspect = vrf::fg::ImageAspect::Color,
                                                                 .clearValue  = vrf::fg::ClearValue::TransparentBlack});
                data.depth  = builder.write(data.depth,
                                           vrf::fg::Attachment {.imageAspect = vrf::fg::ImageAspect::Depth,
                                                                 .clearValue  = vrf::fg::ClearValue::One});
            },
            [&, pushConstants](const GBufferData&, FrameGraphPassResources&, void* ctx) {
                auto& rc = *static_cast<vrf::fg::RenderContext*>(ctx);
                rc.BeginRendering();
                rc.BindPipeline(*gbufferPipeline);
                rc.device.Core().CmdSetConstants(rc.cmd, 0, &pushConstants, sizeof(pushConstants));
                const VriDrawDesc draw {.vertexNum = 36, .instanceNum = 1};
                rc.device.Core().CmdDraw(rc.cmd, &draw);
                rc.EndRendering();
            });
        blackboard.add<GBufferData>() = gbuffer;

        struct LightingData
        {
            FrameGraphResource sceneColor;
        };
        const auto& lighting = graph.addCallbackPass<LightingData>(
            "Lighting",
            [&](FrameGraph::Builder& builder, LightingData& data) {
                const auto& gb = blackboard.get<GBufferData>();
                builder.read(
                    gb.albedo,
                    vrf::fg::TextureRead {
                        .binding     = {.location = {0, 0}, .pipelineStage = vrf::fg::PipelineStage::FragmentShader},
                        .type        = vrf::fg::TextureRead::Type::SampledImage,
                        .imageAspect = vrf::fg::ImageAspect::Color,
                    });
                builder.read(
                    gb.normal,
                    vrf::fg::TextureRead {
                        .binding     = {.location = {0, 1}, .pipelineStage = vrf::fg::PipelineStage::FragmentShader},
                        .type        = vrf::fg::TextureRead::Type::SampledImage,
                        .imageAspect = vrf::fg::ImageAspect::Color,
                    });

                data.sceneColor = builder.create<vrf::fg::FrameGraphTexture>(
                    "SceneColor",
                    {.extent = extent,
                     .format = kHdrFormat,
                     .usage  = VriTextureUsage_ColorAttachment | VriTextureUsage_ShaderResource});
                data.sceneColor = builder.write(data.sceneColor,
                                                vrf::fg::Attachment {.index       = 0,
                                                                     .imageAspect = vrf::fg::ImageAspect::Color,
                                                                     .clearValue  = vrf::fg::ClearValue::OpaqueBlack});
            },
            [&](const LightingData&, FrameGraphPassResources&, void* ctx) {
                auto& rc = *static_cast<vrf::fg::RenderContext*>(ctx);
                rc.SetSampler(0, 2, samplers["linearClamp"]);
                rc.RenderFullScreenPostProcess(*lightingPipeline);
            });
        blackboard.add<LightingData>() = lighting;

        graph.addCallbackPass(
            "Tonemap",
            [&](FrameGraph::Builder& builder, auto&) {
                builder.read(
                    blackboard.get<LightingData>().sceneColor,
                    vrf::fg::TextureRead {
                        .binding     = {.location = {0, 0}, .pipelineStage = vrf::fg::PipelineStage::FragmentShader},
                        .type        = vrf::fg::TextureRead::Type::SampledImage,
                        .imageAspect = vrf::fg::ImageAspect::Color,
                    });
                backbufferId = builder.write(
                    backbufferId, vrf::fg::Attachment {.index = 0, .imageAspect = vrf::fg::ImageAspect::Color});
                builder.setSideEffect(); // present target: never cull
            },
            [&](const auto&, FrameGraphPassResources&, void* ctx) {
                auto& rc = *static_cast<vrf::fg::RenderContext*>(ctx);
                rc.SetSampler(0, 1, samplers["linearClamp"]);
                rc.RenderFullScreenPostProcess(*tonemapPipeline);
            });

        graph.compile();
        graph.execute(&renderContext, &transientResources);

        // Leave the backbuffer presentable.
        renderContext.TransitionTexture(backbuffers[acquired.index],
                                        {VriAccess_None, VriLayout_Present, VriPipelineStage_AllCommands});

        frames.Submit(); // no CPU wait - next iteration records while this frame draws
        swapchain.Present();
        ++frameCount;

        if (autoExit && std::chrono::steady_clock::now() - start > autoExitAfter)
        {
            break;
        }
    }

    device.WaitIdle();

    const auto stats = transientResources.getStats();
    std::printf("[framegraph-deferred] frames: %llu, transient pool: %.2f MiB textures / %.2f KiB buffers\n",
                static_cast<unsigned long long>(frameCount),
                static_cast<double>(stats.textures) / (1024.0 * 1024.0),
                static_cast<double>(stats.buffers) / 1024.0);

    for (auto& [_, sampler] : samplers)
    {
        device.Core().DestroyDescriptor(sampler);
    }
    for (const auto* pipeline : {&*gbufferPipeline, &*lightingPipeline, &*tonemapPipeline})
    {
        device.Core().DestroyPipeline(pipeline->pipeline);
        device.Core().DestroyPipelineLayout(pipeline->layout->handle);
    }
    return 0;
}
