// vrf gaussian_splat - a real 3D Gaussian Splatting renderer on the vrf
// framegraph: each splat is an instanced quad expanded to its projected 2D
// covariance ellipse, gaussian-weighted, premultiplied over-blended back-to-
// front (host CPU depth sort per frame) into a color target, then presented
// directly. This is the core EWA splat raster (Zwicker et al.).
//
// Color is display-referred: SH DC -> 0.5 + C0*dc is already the sRGB display
// value (as in NVIDIA vk_gaussian_splatting), so the present pass copies it
// straight through - NO tonemapping (Reinhard would desaturate/wash it out).
//
// View-dependent SH (bands 0-3) is evaluated per splat. Depth ordering is a
// per-frame CPU sort for now; a GPU radix sort is the next upgrade. Loads a
// .ply/.spz/.splat from argv[1] if given, else generates a synthetic sphere.
//
// Set VRF_EXAMPLE_AUTO_EXIT to run a few seconds and quit (CI/verification).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <vector>

#include <fg/Blackboard.hpp>
#include <fg/FrameGraph.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vrf/asset/gaussian_splat.hpp>
#include <vrf/asset/loaders/gaussian_splat_loader.hpp>
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
    const unsigned char g_splatVshlib[] = {
#include "splat.vshlib.h"
    };

    constexpr float kShC0 = 0.28209479177387814f;

    // Camera UBO - must match Camera in splat.slang (std140-friendly).
    struct CameraBlock
    {
        glm::mat4 view;
        glm::mat4 proj;
        glm::vec3 cameraPos;
        float     splatScale;
        glm::vec2 focal;
        glm::vec2 viewport;
        uint32_t  shDegree;
        uint32_t  shStride; // higher-order floats per point = coeffCount * 3
        glm::vec2 pad0;
    };

    // Rest-SH coefficient count per point (excludes DC), by SH degree.
    uint32_t restCoeffCount(int degree)
    {
        switch (degree)
        {
            case 1:
                return 3;
            case 2:
                return 8;
            case 3:
                return 15;
            default:
                return 0;
        }
    }

    // A synthetic colored sphere of splats: points on a fibonacci sphere, each a
    // small disk tangent to the surface, colored by direction. Enough to show the
    // covariance projection and back-to-front blending working in 3D.
    vrf::GaussianSplat makeSyntheticSphere(uint32_t count)
    {
        vrf::GaussianSplat out;
        out.name      = "synthetic-sphere";
        out.numPoints = static_cast<int32_t>(count);
        out.shDegree  = 1; // one rest band, to exercise view-dependent SH
        out.splats.reserve(count);
        out.sh.assign(static_cast<size_t>(count) * 3 * 3, 0.0f); // 3 coeffs x 3 channels

        const float golden = 3.14159265358979f * (3.0f - std::sqrt(5.0f));
        for (uint32_t i = 0; i < count; ++i)
        {
            const float     y      = 1.0f - (i + 0.5f) / count * 2.0f; // 1..-1
            const float     radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
            const float     theta  = golden * i;
            const glm::vec3 n {std::cos(theta) * radius, y, std::sin(theta) * radius};

            vrf::GaussianSplatPoint p {};
            p.position = n; // unit sphere
            // Tangent-plane disk: two axes ~0.05, thin along the normal.
            p.scale    = glm::vec3 {std::log(0.045f), std::log(0.045f), std::log(0.010f)};
            p.rotation = glm::vec4 {0.0f, 0.0f, 0.0f, 1.0f}; // identity (disk faces +z-ish; fine for a demo)
            p.opacity  = 4.0f;                               // sigmoid ~0.98
            // color = 0.5 + 0.5*normal  =>  shDC = (color - 0.5) / C0
            const glm::vec3 color = 0.5f + 0.5f * n;
            p.shDC                = (color - 0.5f) / kShC0;
            out.splats.push_back(p);

            // Degree-1 SH: put a red/blue tint on the x-direction coefficient
            // (coeff 2, the -x basis) so color visibly shifts as the camera
            // orbits. Layout: [coeff][channel], channel innermost.
            const size_t shBase        = static_cast<size_t>(i) * 3 * 3;
            out.sh[shBase + 2 * 3 + 0] = 0.9f;  // coeff 2, R
            out.sh[shBase + 2 * 3 + 2] = -0.9f; // coeff 2, B
        }
        return out;
    }

    // A host-visible buffer of `bytes`, optionally filled from `init`.
    VriBuffer* makeHostBuffer(const vrf::RenderDevice& device,
                              uint64_t                 bytes,
                              VriBufferUsageFlags      usage,
                              uint32_t                 structureStride,
                              const void*              init)
    {
        const VriCoreInterface& core   = device.Core();
        VriBuffer*              buffer = nullptr;
        const VriBufferDesc     desc {
                .size            = bytes,
                .structureStride = structureStride,
                .usage           = usage,
                .memoryLocation  = VriMemoryLocation_HostUpload,
        };
        if (!vrf::Succeeded(core.CreateBuffer(device.Handle(), &desc, &buffer)))
            return nullptr;
        if (init)
        {
            std::memcpy(core.MapBuffer(buffer, 0, bytes), init, bytes);
            core.UnmapBuffer(buffer);
        }
        return buffer;
    }
} // namespace

int main(int argc, char** argv)
{
    constexpr vrf::Extent2D kExtent {1280, 720};

    // ---- splat data: file (argv[1]) or synthetic sphere -------------------
    vrf::GaussianSplat splat;
    if (argc > 1)
    {
        if (const auto r = vrf::LoadGaussianSplat(argv[1], splat); !r)
        {
            std::fprintf(stderr, "[gaussian-splat] load %s: %s\n", argv[1], r.error().message.c_str());
            return 1;
        }
        std::printf("[gaussian-splat] loaded %s: %d splats (SH degree %d)\n", argv[1], splat.numPoints, splat.shDegree);
    }
    else
    {
        splat = makeSyntheticSphere(16384);
        std::printf("[gaussian-splat] synthetic sphere: %zu splats\n", splat.splats.size());
    }
    const uint32_t splatCount = static_cast<uint32_t>(splat.splats.size());
    if (splatCount == 0)
    {
        std::fprintf(stderr, "[gaussian-splat] no splats\n");
        return 1;
    }

    // ---- window / device / swapchain / frames -----------------------------
    vrf::WindowDesc windowDesc;
    windowDesc.title  = "vrf gaussian_splat";
    windowDesc.extent = kExtent;
    auto windowResult = vrf::Window::Create(windowDesc);
    if (!windowResult)
    {
        std::fprintf(stderr, "[gaussian-splat] window: %s\n", windowResult.error().message.c_str());
        return 1;
    }
    auto& window = *windowResult.value();

    vrf::RenderDeviceDesc deviceDesc;
    deviceDesc.validation = true;
    auto deviceResult     = vrf::RenderDevice::Create(deviceDesc);
    if (!deviceResult)
    {
        std::fprintf(stderr, "[gaussian-splat] device: %s\n", deviceResult.error().message.c_str());
        return 1;
    }
    vrf::RenderDevice&      device = *deviceResult;
    const VriCoreInterface& core   = device.Core();

    vrf::SwapchainDesc swapDesc;
    swapDesc.window = window.Handle();
    swapDesc.extent = kExtent;
    auto swapResult = vrf::Swapchain::Create(device, swapDesc);
    if (!swapResult)
    {
        std::fprintf(stderr, "[gaussian-splat] swapchain: %s\n", swapResult.error().message.c_str());
        return 1;
    }
    vrf::Swapchain& swapchain = *swapResult;

    constexpr uint32_t kFramesInFlight = 2;
    auto               frameResult     = vrf::FrameStream::Create(device, kFramesInFlight);
    if (!frameResult)
    {
        std::fprintf(stderr, "[gaussian-splat] frame stream: %s\n", frameResult.error().message.c_str());
        return 1;
    }
    vrf::FrameStream frames = std::move(*frameResult);

    auto libResult = vrf::ShaderLibrary::LoadFromMemory(g_splatVshlib, sizeof(g_splatVshlib));
    if (!libResult)
    {
        std::fprintf(stderr, "[gaussian-splat] shaders: %s\n", libResult.error().message.c_str());
        return 1;
    }
    vrf::ShaderLibrary shaders = std::move(*libResult);

    vrf::fg::TransientResources               transientResources {device, kFramesInFlight};
    std::vector<vrf::fg::DescriptorAllocator> descriptorAllocators;
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
        descriptorAllocators.emplace_back(device);
    vrf::fg::Samplers samplers;
    {
        VriSamplerDesc s {};
        s.magFilter = s.minFilter = VriFilter_Linear;
        s.addressModeU = s.addressModeV = s.addressModeW = VriAddressMode_ClampToEdge;
        VriDescriptor* sampler                           = nullptr;
        core.CreateSampler(device.Handle(), &s, &sampler);
        samplers["linearClamp"] = sampler;
    }

    constexpr VriFormat kHdrFormat = VriFormat_RGBA16_SFLOAT;

    std::vector<vrf::fg::Texture> backbuffers;
    auto                          rebuildBackbuffers = [&] {
        backbuffers.clear();
        for (uint32_t i = 0; i < swapchain.TextureCount(); ++i)
            backbuffers.push_back(vrf::fg::Texture::Borrow(
                device,
                swapchain.Texture(i),
                {.extent = swapchain.Extent(), .format = swapchain.Format(), .usage = VriTextureUsage_ColorAttachment},
                {VriAccess_None, VriLayout_Undefined, VriPipelineStage_None}));
    };
    rebuildBackbuffers();

    // ---- GPU buffers -------------------------------------------------------
    // Splats: uploaded once (host-visible, read-only in the shader).
    VriBuffer*     splatBuffer = makeHostBuffer(device,
                                            uint64_t {splatCount} * sizeof(vrf::GaussianSplatPoint),
                                            VriBufferUsage_StorageBuffer,
                                            sizeof(vrf::GaussianSplatPoint),
                                            splat.splats.data());
    VriDescriptor* splatView   = nullptr;
    {
        const VriBufferViewDesc v {.buffer = splatBuffer, .viewType = VriDescriptorType_StructuredBuffer};
        core.CreateBufferView(device.Handle(), &v, &splatView);
    }

    // Higher-order SH coefficients (empty clouds still get a 1-float dummy so the
    // binding is always valid; the shader gates reads on shDegree/shStride).
    const uint32_t     shStride = restCoeffCount(splat.shDegree) * 3;
    std::vector<float> shData   = splat.sh.empty() ? std::vector<float> {0.0f} : splat.sh;
    VriBuffer*         shBuffer = makeHostBuffer(
        device, shData.size() * sizeof(float), VriBufferUsage_StorageBuffer, sizeof(float), shData.data());
    VriDescriptor* shView = nullptr;
    {
        const VriBufferViewDesc v {.buffer = shBuffer, .viewType = VriDescriptorType_StructuredBuffer};
        core.CreateBufferView(device.Handle(), &v, &shView);
    }

    // Per-frame-slot: camera UBO + sorted-index buffer (rewritten each frame after Begin()).
    struct PerFrame
    {
        VriBuffer*     camera     = nullptr;
        VriDescriptor* cameraView = nullptr;
        VriBuffer*     indices    = nullptr;
        VriDescriptor* indexView  = nullptr;
    };
    std::vector<PerFrame> perFrame(kFramesInFlight);
    for (auto& pf : perFrame)
    {
        pf.camera = makeHostBuffer(device, sizeof(CameraBlock), VriBufferUsage_ConstantBuffer, 0, nullptr);
        const VriBufferViewDesc cv {.buffer = pf.camera, .viewType = VriDescriptorType_ConstantBuffer};
        core.CreateBufferView(device.Handle(), &cv, &pf.cameraView);

        pf.indices = makeHostBuffer(
            device, uint64_t {splatCount} * sizeof(uint32_t), VriBufferUsage_StorageBuffer, sizeof(uint32_t), nullptr);
        const VriBufferViewDesc iv {.buffer = pf.indices, .viewType = VriDescriptorType_StructuredBuffer};
        core.CreateBufferView(device.Handle(), &iv, &pf.indexView);
    }

    // ---- splat pipeline: instanced quad, premultiplied over-blend ----------
    using Set = vrf::fg::PipelineLayoutInfo::Set;
    const Set splatSet {0,
                        {
                            {.baseRegister   = 0,
                             .descriptorNum  = 1,
                             .descriptorType = VriDescriptorType_ConstantBuffer,
                             .shaderStages   = VriShaderStage_Vertex},
                            {.baseRegister   = 1,
                             .descriptorNum  = 1,
                             .descriptorType = VriDescriptorType_StructuredBuffer,
                             .shaderStages   = VriShaderStage_Vertex},
                            {.baseRegister   = 2,
                             .descriptorNum  = 1,
                             .descriptorType = VriDescriptorType_StructuredBuffer,
                             .shaderStages   = VriShaderStage_Vertex},
                            {.baseRegister   = 3,
                             .descriptorNum  = 1,
                             .descriptorType = VriDescriptorType_StructuredBuffer,
                             .shaderStages   = VriShaderStage_Vertex},
                        }};

    vrf::fg::PassPipeline splatPipeline;
    {
        auto vs = shaders.Resolve("splat", vrf::ShaderStage::Vertex, {});
        auto fs = shaders.Resolve("splat", vrf::ShaderStage::Fragment, {});
        if (!vs || !fs)
        {
            std::fprintf(stderr, "[gaussian-splat] splat shader resolve failed\n");
            return 1;
        }
        vrf::PipelineLayoutBuilder lb;
        lb.SetShaderStages(VriShaderStage_AllGraphics);
        lb.AddDescriptorSet(splatSet.registerSpace, splatSet.ranges);
        auto layout = lb.Build(device);
        if (!layout)
        {
            std::fprintf(stderr, "[gaussian-splat] layout: %s\n", layout.error().message.c_str());
            return 1;
        }

        // Premultiplied over: dst = src.rgb + (1-src.a)*dst; a = src.a + (1-src.a)*dst.a
        const VriBlendDesc blend {.enable   = VRI_TRUE,
                                  .srcColor = VriBlendFactor_One,
                                  .dstColor = VriBlendFactor_OneMinusSrcAlpha,
                                  .colorOp  = VriBlendOp_Add,
                                  .srcAlpha = VriBlendFactor_One,
                                  .dstAlpha = VriBlendFactor_OneMinusSrcAlpha,
                                  .alphaOp  = VriBlendOp_Add};

        auto pipeline = vrf::GraphicsPipelineBuilder {}
                            .SetPipelineLayout(*layout)
                            .AddShader(VriShaderStage_Vertex, vs->spirv, vs->spirvSize, vs->entryPoint.c_str())
                            .AddShader(VriShaderStage_Fragment, fs->spirv, fs->spirvSize, fs->entryPoint.c_str())
                            .SetTopology(VriPrimitiveTopology_TriangleStrip)
                            .SetCullMode(VriCullMode_None)
                            .AddColorAttachment(kHdrFormat, VriColorWrite_RGBA, blend)
                            .Build(device);
        if (!pipeline)
        {
            std::fprintf(stderr, "[gaussian-splat] pipeline: %s\n", pipeline.error().message.c_str());
            return 1;
        }
        auto info     = std::make_shared<vrf::fg::PipelineLayoutInfo>();
        info->handle  = *layout;
        info->sets    = {splatSet};
        splatPipeline = vrf::fg::PassPipeline {*pipeline, std::move(info), false};
    }

    // Tonemap: HDR -> swapchain (Reinhard + gamma), reusing the shared shader id.
    vrf::fg::PassPipeline presentPipeline;
    {
        auto vs = shaders.Resolve("present", vrf::ShaderStage::Vertex, {});
        auto fs = shaders.Resolve("present", vrf::ShaderStage::Fragment, {});
        if (!vs || !fs)
        {
            std::fprintf(stderr, "[gaussian-splat] tonemap shader resolve failed\n");
            return 1;
        }
        const Set                  tset {0,
                                         {
                            {.baseRegister   = 0,
                                              .descriptorNum  = 1,
                                              .descriptorType = VriDescriptorType_Texture,
                                              .shaderStages   = VriShaderStage_Fragment},
                            {.baseRegister   = 1,
                                              .descriptorNum  = 1,
                                              .descriptorType = VriDescriptorType_Sampler,
                                              .shaderStages   = VriShaderStage_Fragment},
                        }};
        vrf::PipelineLayoutBuilder lb;
        lb.SetShaderStages(VriShaderStage_AllGraphics);
        lb.AddDescriptorSet(tset.registerSpace, tset.ranges);
        auto layout   = lb.Build(device);
        auto pipeline = vrf::GraphicsPipelineBuilder {}
                            .SetPipelineLayout(*layout)
                            .AddShader(VriShaderStage_Vertex, vs->spirv, vs->spirvSize, vs->entryPoint.c_str())
                            .AddShader(VriShaderStage_Fragment, fs->spirv, fs->spirvSize, fs->entryPoint.c_str())
                            .SetTopology(VriPrimitiveTopology_TriangleList)
                            .SetCullMode(VriCullMode_None)
                            .AddColorAttachment(swapchain.Format())
                            .Build(device);
        if (!layout || !pipeline)
        {
            std::fprintf(stderr, "[gaussian-splat] tonemap pipeline failed\n");
            return 1;
        }
        auto info       = std::make_shared<vrf::fg::PipelineLayoutInfo>();
        info->handle    = *layout;
        info->sets      = {tset};
        presentPipeline = vrf::fg::PassPipeline {*pipeline, std::move(info), false};
    }

    // ---- frame loop --------------------------------------------------------
    const auto start      = std::chrono::steady_clock::now();
    uint64_t   frameCount = 0;
    const bool autoExit   = std::getenv("VRF_EXAMPLE_AUTO_EXIT") != nullptr;

    std::vector<uint32_t> order(splatCount);
    std::vector<float>    viewZ(splatCount);

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

        VriCommandBuffer* cmd           = frames.Begin();
        auto&             slotAllocator = descriptorAllocators[frames.FrameIndex()];
        PerFrame&         pf            = perFrame[frames.FrameIndex()];

        transientResources.update();
        slotAllocator.Reset();
        vrf::fg::RenderContext renderContext {device, cmd, samplers, slotAllocator};

        const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        const auto  extent  = swapchain.Extent();
        const float aspect  = static_cast<float>(extent.width) / static_cast<float>(extent.height);

        // Orbit camera around the splat cloud.
        const float     angle = seconds * 0.5f;
        const glm::vec3 eye {std::cos(angle) * 3.2f, 0.8f, std::sin(angle) * 3.2f};
        const glm::mat4 view = glm::lookAt(eye, glm::vec3 {0.0f}, glm::vec3 {0.0f, 1.0f, 0.0f});
        const float     fovY = glm::radians(50.0f);
        glm::mat4       proj = glm::perspective(fovY, aspect, 0.05f, 100.0f);
        proj[1][1] *= -1.0f; // Vulkan Y-down clip space

        CameraBlock camBlock {};
        camBlock.view       = view;
        camBlock.proj       = proj;
        camBlock.cameraPos  = eye;
        const float focalY  = 0.5f * static_cast<float>(extent.height) / std::tan(0.5f * fovY);
        camBlock.focal      = glm::vec2 {focalY, focalY};
        camBlock.viewport   = glm::vec2 {static_cast<float>(extent.width), static_cast<float>(extent.height)};
        camBlock.splatScale = 1.0f;
        camBlock.shDegree   = static_cast<uint32_t>(splat.shDegree);
        camBlock.shStride   = shStride;
        std::memcpy(core.MapBuffer(pf.camera, 0, sizeof(camBlock)), &camBlock, sizeof(camBlock));
        core.UnmapBuffer(pf.camera);

        // CPU back-to-front sort by view-space depth (farthest first).
        for (uint32_t i = 0; i < splatCount; ++i)
        {
            const glm::vec3& p = splat.splats[i].position;
            viewZ[i]           = view[0].z * p.x + view[1].z * p.y + view[2].z * p.z + view[3].z;
        }
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return viewZ[a] < viewZ[b]; });
        std::memcpy(core.MapBuffer(pf.indices, 0, order.size() * sizeof(uint32_t)),
                    order.data(),
                    order.size() * sizeof(uint32_t));
        core.UnmapBuffer(pf.indices);

        // ---- graph: splat raster -> tonemap -------------------------------
        FrameGraph           graph;
        FrameGraphBlackboard blackboard;
        auto                 backbufferId = vrf::fg::importTexture(graph, "Backbuffer", &backbuffers[acquired.index]);

        struct SplatData
        {
            FrameGraphResource color;
        };
        const auto& splatPass = graph.addCallbackPass<SplatData>(
            "Splat",
            [&](FrameGraph::Builder& builder, SplatData& data) {
                data.color = builder.create<vrf::fg::FrameGraphTexture>(
                    "SplatColor",
                    {.extent = extent,
                     .format = kHdrFormat,
                     .usage  = VriTextureUsage_ColorAttachment | VriTextureUsage_ShaderResource});
                data.color = builder.write(data.color,
                                           vrf::fg::Attachment {.index       = 0,
                                                                .imageAspect = vrf::fg::ImageAspect::Color,
                                                                .clearValue  = vrf::fg::ClearValue::OpaqueBlack});
            },
            [&](const SplatData&, FrameGraphPassResources&, void* ctx) {
                auto& rc = *static_cast<vrf::fg::RenderContext*>(ctx);
                rc.resourceSet[0][0] =
                    vrf::fg::bindings::RawDescriptor {pf.cameraView, VriDescriptorType_ConstantBuffer};
                rc.resourceSet[0][1] = vrf::fg::bindings::RawDescriptor {splatView, VriDescriptorType_StructuredBuffer};
                rc.resourceSet[0][2] =
                    vrf::fg::bindings::RawDescriptor {pf.indexView, VriDescriptorType_StructuredBuffer};
                rc.resourceSet[0][3] = vrf::fg::bindings::RawDescriptor {shView, VriDescriptorType_StructuredBuffer};
                rc.BeginRendering();
                rc.BindPipeline(splatPipeline);
                const VriDrawDesc draw {.vertexNum = 4, .instanceNum = splatCount};
                rc.device.Core().CmdDraw(rc.cmd, &draw);
                rc.EndRendering();
            });
        blackboard.add<SplatData>() = splatPass;

        graph.addCallbackPass(
            "Tonemap",
            [&](FrameGraph::Builder& builder, auto&) {
                builder.read(
                    blackboard.get<SplatData>().color,
                    vrf::fg::TextureRead {
                        .binding     = {.location = {0, 0}, .pipelineStage = vrf::fg::PipelineStage::FragmentShader},
                        .type        = vrf::fg::TextureRead::Type::SampledImage,
                        .imageAspect = vrf::fg::ImageAspect::Color,
                    });
                backbufferId = builder.write(
                    backbufferId, vrf::fg::Attachment {.index = 0, .imageAspect = vrf::fg::ImageAspect::Color});
                builder.setSideEffect();
            },
            [&](const auto&, FrameGraphPassResources&, void* ctx) {
                auto& rc = *static_cast<vrf::fg::RenderContext*>(ctx);
                rc.SetSampler(0, 1, samplers["linearClamp"]);
                rc.RenderFullScreenPostProcess(presentPipeline);
            });

        graph.compile();
        graph.execute(&renderContext, &transientResources);

        renderContext.TransitionTexture(backbuffers[acquired.index],
                                        {VriAccess_None, VriLayout_Present, VriPipelineStage_AllCommands});
        frames.Submit();
        swapchain.Present();
        ++frameCount;

        if (autoExit && std::chrono::steady_clock::now() - start > std::chrono::seconds(5))
            break;
    }

    device.WaitIdle();
    std::printf("[gaussian-splat] frames: %llu, splats: %u\n", static_cast<unsigned long long>(frameCount), splatCount);

    for (auto& pf : perFrame)
    {
        core.DestroyDescriptor(pf.cameraView);
        core.DestroyDescriptor(pf.indexView);
        core.DestroyBuffer(pf.camera);
        core.DestroyBuffer(pf.indices);
    }
    core.DestroyDescriptor(splatView);
    core.DestroyBuffer(splatBuffer);
    core.DestroyDescriptor(shView);
    core.DestroyBuffer(shBuffer);
    for (auto& [_, sampler] : samplers)
        core.DestroyDescriptor(sampler);
    for (const auto* p : {&splatPipeline, &presentPipeline})
    {
        core.DestroyPipeline(p->pipeline);
        core.DestroyPipelineLayout(p->layout->handle);
    }
    return 0;
}
