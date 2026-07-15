// vrf gaussian_splat - a real 3D Gaussian Splatting renderer on the vrf
// framegraph: each splat is an instanced quad expanded to its projected 2D
// covariance ellipse, gaussian-weighted, premultiplied over-blended back-to-
// front into a color target, then presented directly. This is the core EWA
// splat raster (Zwicker et al.).
//
// Color is display-referred: SH DC -> 0.5 + C0*dc is already the sRGB display
// value (as in NVIDIA vk_gaussian_splatting), so the present pass copies it
// straight through - NO tonemapping (Reinhard would desaturate/wash it out).
//
// View-dependent SH (bands 0-3) is evaluated per splat. Depth ordering is a
// GPU counting sort over 16-bit quantized depth (splat_sort.* + the new
// ComputePipelineBuilder), recorded before the raster each frame - no CPU sort.
// Loads a .ply/.spz/.splat from argv[1], else generates a synthetic sphere.
//
// Set VRF_EXAMPLE_AUTO_EXIT to run a few seconds and quit (CI/verification).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
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
#include <vrf/gpu/builders/compute_pipeline_builder.hpp>
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

    // Push constant for the GPU sort - must match SortParams in sort_common.slangh.
    struct SortParams
    {
        glm::mat4 view;
        glm::mat4 proj;
        uint32_t  count;
        float     nearZ;
        float     farZ;
        float     dilation;
        float     focalY;
        float     minPixels;
        float     pad0;
        float     pad1;
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

    // GPU depth sort scratch (per frame-in-flight slot): 16-bit counting sort.
    constexpr uint32_t kSortBuckets = 65536;
    constexpr uint32_t kSortBlocks  = kSortBuckets / 256;

    // Per-frame-slot: camera UBO + sorted-index buffer + sort scratch (all
    // rewritten each frame after Begin(), which waited for this slot's prev frame).
    struct PerFrame
    {
        VriBuffer*     camera       = nullptr;
        VriDescriptor* cameraView   = nullptr;
        VriBuffer*     indices      = nullptr; // sorted output (= scatter target)
        VriDescriptor* indexView    = nullptr;
        VriBuffer*     histo        = nullptr;
        VriDescriptor* histoView    = nullptr;
        VriBuffer*     blockSum     = nullptr;
        VriDescriptor* blockSumView = nullptr;
        VriBuffer*     blockOff     = nullptr;
        VriDescriptor* blockOffView = nullptr;
        VriBuffer*     offset       = nullptr;
        VriDescriptor* offsetView   = nullptr;
        VriBuffer*     bucket       = nullptr;
        VriDescriptor* bucketView   = nullptr;
        VriBuffer*     drawArgs     = nullptr; // VriDrawDesc for CmdDrawIndirect
        VriDescriptor* drawArgsView = nullptr;
    };
    auto makeScratch = [&](uint64_t elems, VriBuffer*& buf, VriDescriptor*& view) {
        buf = makeHostBuffer(device, elems * sizeof(uint32_t), VriBufferUsage_StorageBuffer, sizeof(uint32_t), nullptr);
        const VriBufferViewDesc v {.buffer = buf, .viewType = VriDescriptorType_StorageBuffer};
        core.CreateBufferView(device.Handle(), &v, &view);
    };
    std::vector<PerFrame> perFrame(kFramesInFlight);
    for (auto& pf : perFrame)
    {
        pf.camera = makeHostBuffer(device, sizeof(CameraBlock), VriBufferUsage_ConstantBuffer, 0, nullptr);
        const VriBufferViewDesc cv {.buffer = pf.camera, .viewType = VriDescriptorType_ConstantBuffer};
        core.CreateBufferView(device.Handle(), &cv, &pf.cameraView);

        // indices is both the render input (StructuredBuffer) and scatter output (StorageBuffer).
        pf.indices = makeHostBuffer(
            device, uint64_t {splatCount} * sizeof(uint32_t), VriBufferUsage_StorageBuffer, sizeof(uint32_t), nullptr);
        const VriBufferViewDesc iv {.buffer = pf.indices, .viewType = VriDescriptorType_StructuredBuffer};
        core.CreateBufferView(device.Handle(), &iv, &pf.indexView);

        makeScratch(kSortBuckets, pf.histo, pf.histoView);
        makeScratch(kSortBlocks, pf.blockSum, pf.blockSumView);
        makeScratch(kSortBlocks, pf.blockOff, pf.blockOffView);
        makeScratch(kSortBuckets, pf.offset, pf.offsetView);
        makeScratch(splatCount, pf.bucket, pf.bucketView);

        // Draw args: written by the sort (storage), read by CmdDrawIndirect.
        pf.drawArgs = makeHostBuffer(device,
                                     4 * sizeof(uint32_t),
                                     VriBufferUsage_StorageBuffer | VriBufferUsage_IndirectBuffer,
                                     sizeof(uint32_t),
                                     nullptr);
        const VriBufferViewDesc dv {.buffer = pf.drawArgs, .viewType = VriDescriptorType_StorageBuffer};
        core.CreateBufferView(device.Handle(), &dv, &pf.drawArgsView);
    }

    // A StorageBuffer view of the sorted-index buffer for the scatter stage
    // (pf.indexView above is a StructuredBuffer view for the vertex shader).
    std::vector<VriDescriptor*> sortedRWView(kFramesInFlight, nullptr);
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        const VriBufferViewDesc v {.buffer = perFrame[i].indices, .viewType = VriDescriptorType_StorageBuffer};
        core.CreateBufferView(device.Handle(), &v, &sortedRWView[i]);
    }
    // Read-only structured view of the splat buffer for the sort (binding 0).
    VriDescriptor* splatSortView = nullptr;
    {
        const VriBufferViewDesc v {.buffer = splatBuffer, .viewType = VriDescriptorType_StructuredBuffer};
        core.CreateBufferView(device.Handle(), &v, &splatSortView);
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

    // ---- GPU sort: one layout (7 storage buffers + push constant), 6 pipelines --
    auto sortLayoutInfo  = std::make_shared<vrf::fg::PipelineLayoutInfo>();
    sortLayoutInfo->sets = {Set {0,
                                 {
                                     {.baseRegister   = 0,
                                      .descriptorNum  = 1,
                                      .descriptorType = VriDescriptorType_StructuredBuffer,
                                      .shaderStages   = VriShaderStage_Compute},
                                     {.baseRegister   = 1,
                                      .descriptorNum  = 1,
                                      .descriptorType = VriDescriptorType_StorageBuffer,
                                      .shaderStages   = VriShaderStage_Compute},
                                     {.baseRegister   = 2,
                                      .descriptorNum  = 1,
                                      .descriptorType = VriDescriptorType_StorageBuffer,
                                      .shaderStages   = VriShaderStage_Compute},
                                     {.baseRegister   = 3,
                                      .descriptorNum  = 1,
                                      .descriptorType = VriDescriptorType_StorageBuffer,
                                      .shaderStages   = VriShaderStage_Compute},
                                     {.baseRegister   = 4,
                                      .descriptorNum  = 1,
                                      .descriptorType = VriDescriptorType_StorageBuffer,
                                      .shaderStages   = VriShaderStage_Compute},
                                     {.baseRegister   = 5,
                                      .descriptorNum  = 1,
                                      .descriptorType = VriDescriptorType_StorageBuffer,
                                      .shaderStages   = VriShaderStage_Compute},
                                     {.baseRegister   = 6,
                                      .descriptorNum  = 1,
                                      .descriptorType = VriDescriptorType_StorageBuffer,
                                      .shaderStages   = VriShaderStage_Compute},
                                     {.baseRegister   = 7,
                                      .descriptorNum  = 1,
                                      .descriptorType = VriDescriptorType_StorageBuffer,
                                      .shaderStages   = VriShaderStage_Compute},
                                 }}};
    {
        std::vector<VriDescriptorSetDesc> sets;
        sets.push_back({.registerSpace = 0,
                        .ranges        = sortLayoutInfo->sets[0].ranges.data(),
                        .rangeNum      = static_cast<uint32_t>(sortLayoutInfo->sets[0].ranges.size())});
        const VriPushConstantDesc push {
            .baseRegister = 0, .size = sizeof(SortParams), .shaderStages = VriShaderStage_Compute};
        const VriPipelineLayoutDesc ld {.descriptorSets   = sets.data(),
                                        .descriptorSetNum = 1,
                                        .pushConstants    = &push,
                                        .pushConstantNum  = 1,
                                        .shaderStages     = VriShaderStage_Compute};
        if (!vrf::Succeeded(core.CreatePipelineLayout(device.Handle(), &ld, &sortLayoutInfo->handle)))
        {
            std::fprintf(stderr, "[gaussian-splat] sort layout failed\n");
            return 1;
        }
    }
    const char* kSortStages[6] = {
        "sort_clear", "sort_histogram", "sort_scanlocal", "sort_scanblocks", "sort_buildoffset", "sort_scatter"};
    VriPipeline* sortPipelines[6] {};
    for (int s = 0; s < 6; ++s)
    {
        auto cs = shaders.Resolve(kSortStages[s], vrf::ShaderStage::Compute, {});
        if (!cs)
        {
            std::fprintf(stderr, "[gaussian-splat] sort shader '%s' resolve failed\n", kSortStages[s]);
            return 1;
        }
        auto p = vrf::ComputePipelineBuilder {}
                     .SetPipelineLayout(sortLayoutInfo->handle)
                     .SetShader(cs->spirv, cs->spirvSize, cs->entryPoint.c_str())
                     .Build(device);
        if (!p)
        {
            std::fprintf(
                stderr, "[gaussian-splat] sort pipeline '%s': %s\n", kSortStages[s], p.error().message.c_str());
            return 1;
        }
        sortPipelines[s] = *p;
    }

    // Record the 6-stage sort into `cmd`, writing sorted indices into slot `pf`.
    auto recordSort =
        [&](VriCommandBuffer* cmd, vrf::fg::RenderContext& rc, PerFrame& pf, uint32_t slot, const SortParams& sp) {
            // Compute-to-compute RAW/WAR barrier over every sort scratch buffer.
            VriBuffer* const sortBufs[7] = {
                pf.histo, pf.blockSum, pf.blockOff, pf.offset, pf.bucket, pf.indices, pf.drawArgs};
            const auto barrier = [&] {
                VriBufferBarrierDesc bb[7];
                for (int k = 0; k < 7; ++k)
                    bb[k] = {.buffer = sortBufs[k],
                             .before = {VriAccess_ShaderResourceStorageWrite, VriPipelineStage_ComputeShader},
                             .after  = {VriAccess_ShaderResourceStorageRead | VriAccess_ShaderResourceStorageWrite,
                                        VriPipelineStage_ComputeShader}};
                const VriBarrierGroupDesc bg {.buffers = bb, .bufferNum = 7};
                core.CmdBarrier(cmd, &bg);
            };
            core.CmdSetPipelineLayout(cmd, sortLayoutInfo->handle);
            rc.resourceSet[0][0] = vrf::fg::bindings::RawDescriptor {splatSortView, VriDescriptorType_StructuredBuffer};
            rc.resourceSet[0][1] = vrf::fg::bindings::RawDescriptor {pf.histoView, VriDescriptorType_StorageBuffer};
            rc.resourceSet[0][2] = vrf::fg::bindings::RawDescriptor {pf.blockSumView, VriDescriptorType_StorageBuffer};
            rc.resourceSet[0][3] = vrf::fg::bindings::RawDescriptor {pf.blockOffView, VriDescriptorType_StorageBuffer};
            rc.resourceSet[0][4] = vrf::fg::bindings::RawDescriptor {pf.offsetView, VriDescriptorType_StorageBuffer};
            rc.resourceSet[0][5] = vrf::fg::bindings::RawDescriptor {pf.bucketView, VriDescriptorType_StorageBuffer};
            rc.resourceSet[0][6] =
                vrf::fg::bindings::RawDescriptor {sortedRWView[slot], VriDescriptorType_StorageBuffer};
            rc.resourceSet[0][7] = vrf::fg::bindings::RawDescriptor {pf.drawArgsView, VriDescriptorType_StorageBuffer};
            core.CmdSetPipeline(cmd, sortPipelines[0]);
            rc.BindPipeline(vrf::fg::PassPipeline {sortPipelines[0], sortLayoutInfo, true});
            core.CmdSetConstants(cmd, 0, &sp, sizeof(sp));

            const uint32_t        bucketGroups = kSortBuckets / 256;
            const uint32_t        splatGroups  = (sp.count + 255) / 256;
            const VriDispatchDesc dClear {.x = bucketGroups, .y = 1, .z = 1};
            core.CmdDispatch(cmd, &dClear);
            barrier();
            core.CmdSetPipeline(cmd, sortPipelines[1]); // histogram
            const VriDispatchDesc dHist {.x = splatGroups, .y = 1, .z = 1};
            core.CmdDispatch(cmd, &dHist);
            barrier();
            core.CmdSetPipeline(cmd, sortPipelines[2]); // scanLocal
            core.CmdDispatch(cmd, &dClear);             // 256 blocks = bucketGroups
            barrier();
            core.CmdSetPipeline(cmd, sortPipelines[3]); // scanBlocks (1 group)
            const VriDispatchDesc dOne {.x = 1, .y = 1, .z = 1};
            core.CmdDispatch(cmd, &dOne);
            barrier();
            core.CmdSetPipeline(cmd, sortPipelines[4]); // buildOffset
            core.CmdDispatch(cmd, &dClear);
            barrier();
            core.CmdSetPipeline(cmd, sortPipelines[5]); // scatter
            core.CmdDispatch(cmd, &dHist);
            barrier();
        };

    bool sortValidated = false; // frame-0 GPU-vs-CPU check

    // ---- frame loop --------------------------------------------------------
    const auto start      = std::chrono::steady_clock::now();
    uint64_t   frameCount = 0;
    const bool autoExit   = std::getenv("VRF_EXAMPLE_AUTO_EXIT") != nullptr;

    std::vector<uint32_t> order(splatCount);
    std::vector<float>    viewZ(splatCount);

    // Scene bounds (one-time) for the sort's depth quantization range.
    glm::vec3 sceneMin {std::numeric_limits<float>::max()};
    glm::vec3 sceneMax {std::numeric_limits<float>::lowest()};
    for (const auto& s : splat.splats)
    {
        sceneMin = glm::min(sceneMin, s.position);
        sceneMax = glm::max(sceneMax, s.position);
    }
    const glm::vec3 sceneCenter = 0.5f * (sceneMin + sceneMax);
    const float     sceneRadius = 0.5f * glm::length(sceneMax - sceneMin) + 1e-3f;

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

        // GPU back-to-front sort: 6-stage counting sort over quantized depth,
        // recorded into cmd before the graph's splat pass reads pf.indices.
        const float distToCenter = glm::length(eye - sceneCenter);
        SortParams  sp {};
        sp.view      = view;
        sp.proj      = proj;
        sp.count     = splatCount;
        sp.nearZ     = std::max(0.01f, distToCenter - sceneRadius);
        sp.farZ      = distToCenter + sceneRadius;
        sp.dilation  = 0.2f;   // 20% frustum slack so partly-onscreen splats survive
        sp.focalY    = focalY; // for the projected-size cull
        sp.minPixels = 0.0f;   // 0 = size cull off; frustum cull still active
        recordSort(cmd, renderContext, pf, frames.FrameIndex(), sp);

        // sort (compute-write) -> vertex-shader read of the indices + indirect read of the draw args.
        {
            const VriBufferBarrierDesc bb[2] = {
                {.buffer = pf.indices,
                 .before = {VriAccess_ShaderResourceStorageWrite, VriPipelineStage_ComputeShader},
                 .after  = {VriAccess_ShaderResourceRead, VriPipelineStage_VertexShader}},
                {.buffer = pf.drawArgs,
                 .before = {VriAccess_ShaderResourceStorageWrite, VriPipelineStage_ComputeShader},
                 .after  = {VriAccess_IndirectBufferRead, VriPipelineStage_DrawIndirect}}};
            const VriBarrierGroupDesc bg {.buffers = bb, .bufferNum = 2};
            core.CmdBarrier(cmd, &bg);
        }

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
                // Instance count = visible splats, written into the draw args by the sort's
                // frustum/size cull (CmdDrawIndirect reads it on the GPU - no CPU stall).
                rc.device.Core().CmdDrawIndirect(rc.cmd, pf.drawArgs, 0, 1, 4 * sizeof(uint32_t));
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

        // Frame-0 validation: the GPU sort must produce back-to-front order.
        // Verify view-space depth is monotonically non-decreasing (far -> near).
        if (!sortValidated)
        {
            sortValidated = true;
            device.WaitIdle();
            // Only the visible (non-culled) prefix [0, visibleCount) is valid.
            const auto*    args = static_cast<const uint32_t*>(core.MapBuffer(pf.drawArgs, 0, 4 * sizeof(uint32_t)));
            const uint32_t visibleCount = args[1];
            core.UnmapBuffer(pf.drawArgs);
            const auto* sorted =
                static_cast<const uint32_t*>(core.MapBuffer(pf.indices, 0, uint64_t {splatCount} * sizeof(uint32_t)));
            // Check bucket-level monotonicity (intra-bucket order is arbitrary by
            // design and visually irrelevant - buckets are sub-pixel depth slices).
            const auto bucketOf = [&](uint32_t idx) {
                const glm::vec3& p    = splat.splats[idx].position;
                const float      vz   = view[0].z * p.x + view[1].z * p.y + view[2].z * p.z + view[3].z;
                const float      dist = -vz;
                const float      t    = std::clamp((dist - sp.nearZ) / std::max(sp.farZ - sp.nearZ, 1e-6f), 0.0f, 1.0f);
                return static_cast<uint32_t>(std::clamp((1.0f - t) * 65535.0f, 0.0f, 65535.0f));
            };
            uint32_t bucketInversions = 0;
            uint32_t prevBucket       = 0;
            for (uint32_t k = 0; k < visibleCount; ++k)
            {
                const uint32_t b = bucketOf(sorted[k]);
                if (b < prevBucket)
                    ++bucketInversions;
                prevBucket = b;
            }
            core.UnmapBuffer(pf.indices);
            std::printf("[gaussian-splat] GPU sort+cull: %u visible / %u splats, %u bucket-order inversions "
                        "(0 = correct back-to-front)\n",
                        visibleCount,
                        splatCount,
                        bucketInversions);
        }

        if (autoExit && std::chrono::steady_clock::now() - start > std::chrono::seconds(5))
            break;
    }

    device.WaitIdle();
    std::printf("[gaussian-splat] frames: %llu, splats: %u\n", static_cast<unsigned long long>(frameCount), splatCount);

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        PerFrame& pf = perFrame[i];
        core.DestroyDescriptor(pf.cameraView);
        core.DestroyDescriptor(pf.indexView);
        core.DestroyDescriptor(pf.histoView);
        core.DestroyDescriptor(pf.blockSumView);
        core.DestroyDescriptor(pf.blockOffView);
        core.DestroyDescriptor(pf.offsetView);
        core.DestroyDescriptor(pf.bucketView);
        core.DestroyDescriptor(sortedRWView[i]);
        core.DestroyBuffer(pf.camera);
        core.DestroyBuffer(pf.indices);
        core.DestroyBuffer(pf.histo);
        core.DestroyBuffer(pf.blockSum);
        core.DestroyBuffer(pf.blockOff);
        core.DestroyBuffer(pf.offset);
        core.DestroyDescriptor(pf.drawArgsView);
        core.DestroyBuffer(pf.bucket);
        core.DestroyBuffer(pf.drawArgs);
    }
    core.DestroyDescriptor(splatView);
    core.DestroyDescriptor(splatSortView);
    core.DestroyBuffer(splatBuffer);
    core.DestroyDescriptor(shView);
    core.DestroyBuffer(shBuffer);
    for (VriPipeline* p : sortPipelines)
        core.DestroyPipeline(p);
    core.DestroyPipelineLayout(sortLayoutInfo->handle);
    for (auto& [_, sampler] : samplers)
        core.DestroyDescriptor(sampler);
    for (const auto* p : {&splatPipeline, &presentPipeline})
    {
        core.DestroyPipeline(p->pipeline);
        core.DestroyPipelineLayout(p->layout->handle);
    }
    return 0;
}
