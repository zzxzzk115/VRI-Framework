// vrf gaussian_rt - a 3D Gaussian ray tracer (3DGRT) on the vrf inline ray-query
// path. A compute pass emits one world-space AABB per splat; those become a
// procedural BLAS (VRI's AABB acceleration-structure geometry). A ray-query
// compute shader casts a primary ray per pixel, evaluates each splat's gaussian
// response at the ray's closest approach, and blends front-to-back. Presented
// directly (display-referred color, no tonemap), like the raster gaussian_splat.
//
// Reference: NVIDIA vk_gaussian_splatting (VK3DGRT). Runs on any backend with
// hasRayQuery - Vulkan (MoltenVK) and the native Metal backend. Loads a
// .ply/.spz/.splat from argv[1], else a synthetic sphere. VRF_EXAMPLE_AUTO_EXIT
// runs a few seconds and quits.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <vrf/asset/gaussian_splat.hpp>
#include <vrf/asset/loaders/gaussian_splat_loader.hpp>
#include <vrf/fg/render_context.hpp>
#include <vrf/fg/texture.hpp>
#include <vrf/gpu/builders/compute_pipeline_builder.hpp>
#include <vrf/gpu/builders/graphics_pipeline_builder.hpp>
#include <vrf/gpu/builders/pipeline_layout_builder.hpp>
#include <vrf/gpu/frame_stream.hpp>
#include <vrf/gpu/raytracing.hpp>
#include <vrf/gpu/render_device.hpp>
#include <vrf/gpu/screenshot.hpp>
#include <vrf/gpu/shader_library.hpp>
#include <vrf/gpu/swapchain.hpp>
#include <vrf/platform/window.hpp>

namespace
{
    const unsigned char g_vshlib[] = {
#include "gaussian_rt.vshlib.h"
    };

    constexpr float kShC0 = 0.28209479177387814f;

    struct GenParams
    {
        uint32_t count;
    };
    struct CameraBlock
    {
        glm::mat4  invViewProj;
        glm::vec3  cameraPos;
        float      pad0;
        glm::uvec2 resolution;
        glm::uvec2 pad1;
    };

    vrf::GaussianSplat makeSyntheticSphere(uint32_t count)
    {
        vrf::GaussianSplat out;
        out.numPoints = static_cast<int32_t>(count);
        out.splats.reserve(count);
        const float golden = 3.14159265358979f * (3.0f - std::sqrt(5.0f));
        for (uint32_t i = 0; i < count; ++i)
        {
            const float             y      = 1.0f - (i + 0.5f) / count * 2.0f;
            const float             radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
            const float             theta  = golden * i;
            const glm::vec3         n {std::cos(theta) * radius, y, std::sin(theta) * radius};
            vrf::GaussianSplatPoint p {};
            p.position            = n;
            p.scale               = glm::vec3 {std::log(0.05f)};
            p.rotation            = glm::vec4 {0.0f, 0.0f, 0.0f, 1.0f};
            p.opacity             = 4.0f;
            const glm::vec3 color = 0.5f + 0.5f * n;
            p.shDC                = (color - 0.5f) / kShC0;
            out.splats.push_back(p);
        }
        return out;
    }

    VriBuffer* makeBuffer(const vrf::RenderDevice& device,
                          uint64_t                 bytes,
                          VriBufferUsageFlags      usage,
                          uint32_t                 stride,
                          VriMemoryLocation        loc,
                          const void*              init)
    {
        const VriCoreInterface& core   = device.Core();
        VriBuffer*              buffer = nullptr;
        const VriBufferDesc     desc {.size = bytes, .structureStride = stride, .usage = usage, .memoryLocation = loc};
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

    vrf::GaussianSplat splat;
    if (argc > 1)
    {
        if (const auto r = vrf::LoadGaussianSplat(argv[1], splat); !r)
        {
            std::fprintf(stderr, "[gaussian-rt] load %s: %s\n", argv[1], r.error().message.c_str());
            return 1;
        }
        std::printf("[gaussian-rt] loaded %s: %d splats\n", argv[1], splat.numPoints);
    }
    else
    {
        splat = makeSyntheticSphere(8192);
        std::printf("[gaussian-rt] synthetic sphere: %zu splats\n", splat.splats.size());
    }
    const uint32_t splatCount = static_cast<uint32_t>(splat.splats.size());
    if (splatCount == 0)
        return 1;

    // ---- window / device / swapchain / frames -----------------------------
    vrf::WindowDesc windowDesc;
    windowDesc.title  = "vrf gaussian_rt";
    windowDesc.extent = kExtent;
    auto windowResult = vrf::Window::Create(windowDesc);
    if (!windowResult)
    {
        std::fprintf(stderr, "[gaussian-rt] window: %s\n", windowResult.error().message.c_str());
        return 1;
    }
    auto& window = *windowResult.value();

    vrf::RenderDeviceDesc deviceDesc;
    deviceDesc.validation      = true;
    deviceDesc.enabledFeatures = VriFeature_RayQuery;
    auto deviceResult          = vrf::RenderDevice::Create(deviceDesc);
    if (!deviceResult)
    {
        std::fprintf(stderr, "[gaussian-rt] device: %s\n", deviceResult.error().message.c_str());
        return 1;
    }
    vrf::RenderDevice&      device = *deviceResult;
    const VriCoreInterface& core   = device.Core();

    if (device.Desc()->hasRayQuery == VRI_FALSE)
    {
        std::printf("[gaussian-rt] device has no ray query support - skipped.\n");
        return 0;
    }

    vrf::SwapchainDesc swapDesc;
    swapDesc.window = window.Handle();
    swapDesc.extent = kExtent;
    auto swapResult = vrf::Swapchain::Create(device, swapDesc);
    if (!swapResult)
        return 1;
    vrf::Swapchain& swapchain = *swapResult;

    constexpr uint32_t kFramesInFlight = 2;
    auto               frameResult     = vrf::FrameStream::Create(device, kFramesInFlight);
    if (!frameResult)
        return 1;
    vrf::FrameStream frames = std::move(*frameResult);

    auto libResult = vrf::ShaderLibrary::LoadFromMemory(g_vshlib, sizeof(g_vshlib));
    if (!libResult)
        return 1;
    vrf::ShaderLibrary shaders = std::move(*libResult);

    std::vector<vrf::fg::DescriptorAllocator> allocators;
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
        allocators.emplace_back(device);
    vrf::fg::Samplers samplers;
    {
        VriSamplerDesc s {};
        s.magFilter = s.minFilter = VriFilter_Linear;
        s.addressModeU = s.addressModeV = s.addressModeW = VriAddressMode_ClampToEdge;
        VriDescriptor* sampler                           = nullptr;
        core.CreateSampler(device.Handle(), &s, &sampler);
        samplers["linearClamp"] = sampler;
    }

    // ---- splat buffer + AABB buffer ---------------------------------------
    VriBuffer*     splatBuffer = makeBuffer(device,
                                        uint64_t {splatCount} * sizeof(vrf::GaussianSplatPoint),
                                        VriBufferUsage_StorageBuffer,
                                        sizeof(vrf::GaussianSplatPoint),
                                        VriMemoryLocation_HostUpload,
                                        splat.splats.data());
    VriDescriptor* splatView   = nullptr;
    {
        const VriBufferViewDesc v {.buffer = splatBuffer, .viewType = VriDescriptorType_StructuredBuffer};
        core.CreateBufferView(device.Handle(), &v, &splatView);
    }

    VriBuffer*     aabbBuffer = makeBuffer(device,
                                       uint64_t {splatCount} * 6 * sizeof(float),
                                       VriBufferUsage_StorageBuffer | VriBufferUsage_AccelerationBuildInput,
                                       sizeof(float),
                                       VriMemoryLocation_Device,
                                       nullptr);
    VriDescriptor* aabbView   = nullptr;
    {
        const VriBufferViewDesc v {.buffer = aabbBuffer, .viewType = VriDescriptorType_StorageBuffer};
        core.CreateBufferView(device.Handle(), &v, &aabbView);
    }

    // ---- pipelines: aabb_gen (compute), gaussian_rt (compute), present -----
    using Set              = vrf::fg::PipelineLayoutInfo::Set;
    auto makeComputeLayout = [&](const Set& set, uint32_t pushSize) {
        auto info  = std::make_shared<vrf::fg::PipelineLayoutInfo>();
        info->sets = {set};
        std::vector<VriDescriptorSetDesc> sets;
        sets.push_back({.registerSpace = 0,
                        .ranges        = info->sets[0].ranges.data(),
                        .rangeNum      = static_cast<uint32_t>(info->sets[0].ranges.size())});
        const VriPushConstantDesc   push {.baseRegister = 0, .size = pushSize, .shaderStages = VriShaderStage_Compute};
        const VriPipelineLayoutDesc ld {.descriptorSets   = sets.data(),
                                        .descriptorSetNum = 1,
                                        .pushConstants    = &push,
                                        .pushConstantNum  = 1,
                                        .shaderStages     = VriShaderStage_Compute};
        core.CreatePipelineLayout(device.Handle(), &ld, &info->handle);
        return info;
    };
    auto makeCompute = [&](const char* id, const std::shared_ptr<vrf::fg::PipelineLayoutInfo>& info) -> VriPipeline* {
        auto cs = shaders.Resolve(id, vrf::ShaderStage::Compute, {});
        if (!cs)
        {
            std::fprintf(stderr, "[gaussian-rt] shader '%s' resolve failed\n", id);
            std::exit(1);
        }
        auto p = vrf::ComputePipelineBuilder {}
                     .SetPipelineLayout(info->handle)
                     .SetShader(cs->spirv, cs->spirvSize, cs->entryPoint.c_str())
                     .Build(device);
        if (!p)
        {
            std::fprintf(stderr, "[gaussian-rt] pipeline '%s': %s\n", id, p.error().message.c_str());
            std::exit(1);
        }
        return *p;
    };

    auto         genLayout   = makeComputeLayout(Set {0,
                                                      {{.baseRegister   = 0,
                                                        .descriptorNum  = 1,
                                                        .descriptorType = VriDescriptorType_StructuredBuffer,
                                                        .shaderStages   = VriShaderStage_Compute},
                                                       {.baseRegister   = 1,
                                                        .descriptorNum  = 1,
                                                        .descriptorType = VriDescriptorType_StorageBuffer,
                                                        .shaderStages   = VriShaderStage_Compute}}},
                                       sizeof(GenParams));
    VriPipeline* genPipeline = makeCompute("aabb_gen", genLayout);

    auto         rtLayout   = makeComputeLayout(Set {0,
                                                     {{.baseRegister   = 0,
                                                       .descriptorNum  = 1,
                                                       .descriptorType = VriDescriptorType_AccelerationStructure,
                                                       .shaderStages   = VriShaderStage_Compute},
                                                      {.baseRegister   = 1,
                                                       .descriptorNum  = 1,
                                                       .descriptorType = VriDescriptorType_StructuredBuffer,
                                                       .shaderStages   = VriShaderStage_Compute},
                                                      {.baseRegister   = 2,
                                                       .descriptorNum  = 1,
                                                       .descriptorType = VriDescriptorType_StorageTexture,
                                                       .shaderStages   = VriShaderStage_Compute}}},
                                      sizeof(CameraBlock));
    VriPipeline* rtPipeline = makeCompute("gaussian_rt", rtLayout);

    vrf::fg::PassPipeline presentPipeline;
    {
        auto                       vs = shaders.Resolve("present", vrf::ShaderStage::Vertex, {});
        auto                       fs = shaders.Resolve("present", vrf::ShaderStage::Fragment, {});
        const Set                  tset {0,
                                         {{.baseRegister   = 0,
                                           .descriptorNum  = 1,
                                           .descriptorType = VriDescriptorType_Texture,
                                           .shaderStages   = VriShaderStage_Fragment},
                                          {.baseRegister   = 1,
                                           .descriptorNum  = 1,
                                           .descriptorType = VriDescriptorType_Sampler,
                                           .shaderStages   = VriShaderStage_Fragment}}};
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
        auto info       = std::make_shared<vrf::fg::PipelineLayoutInfo>();
        info->handle    = *layout;
        info->sets      = {tset};
        presentPipeline = vrf::fg::PassPipeline {*pipeline, std::move(info), false};
    }

    // ---- procedural BLAS/TLAS (built once - static geometry) --------------
    auto blas = vrf::Blas::CreateAabbs(device, {VriAsAabbsDesc {.buffer = aabbBuffer, .count = splatCount}});
    if (!blas)
    {
        std::fprintf(stderr, "[gaussian-rt] BLAS: %s\n", blas.error().message.c_str());
        return 1;
    }
    auto tlas = vrf::Tlas::Create(device, 1);
    if (!tlas)
        return 1;
    const vrf::AsInstance instance = vrf::AsInstance::Make(vrf::Mat4 {1.0f}, blas->DeviceAddress());
    tlas->SetInstances(&instance, 1);

    // Output storage texture (ray-query target), one per frame slot.
    std::vector<vrf::fg::Texture> outTex;
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
        outTex.push_back(*vrf::fg::Texture::Create(
            device,
            {.extent = kExtent,
             .format = VriFormat_RGBA16_SFLOAT,
             .usage  = VriTextureUsage_ShaderResourceStorage | VriTextureUsage_ShaderResource |
                      VriTextureUsage_TransferSrc})); // TransferSrc: headless screenshot readback

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

    // Camera framing. Real 3DGS captures carry sparse far-background splats that
    // blow up the bounding box (framing the whole cloud as a distant ball), so
    // frame on a robust core instead: per-axis median center + the 85th-percentile
    // distance from it. Ignores outliers; for the synthetic sphere it's ~the radius.
    glm::vec3 center;
    float     radius;
    {
        const size_t       n = splat.splats.size();
        std::vector<float> xs(n), ys(n), zs(n);
        for (size_t i = 0; i < n; ++i)
        {
            xs[i] = splat.splats[i].position.x;
            ys[i] = splat.splats[i].position.y;
            zs[i] = splat.splats[i].position.z;
        }
        const auto median = [](std::vector<float>& v) {
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            return v[v.size() / 2];
        };
        center = glm::vec3 {median(xs), median(ys), median(zs)};
        std::vector<float> dist(n);
        for (size_t i = 0; i < n; ++i)
            dist[i] = glm::length(splat.splats[i].position - center);
        const size_t k = static_cast<size_t>(static_cast<double>(n) * 0.85);
        std::nth_element(dist.begin(), dist.begin() + static_cast<std::ptrdiff_t>(k), dist.end());
        radius = dist[k] + 1e-3f;
    }

    bool        builtAs    = false;
    const auto  start      = std::chrono::steady_clock::now();
    uint64_t    frameCount = 0;
    const bool  autoExit   = std::getenv("VRF_EXAMPLE_AUTO_EXIT") != nullptr;
    const char* shotPath   = std::getenv("VRF_EXAMPLE_SCREENSHOT"); // headless PNG readback
    uint32_t    lastOut    = 0;

    while (!window.ShouldClose())
    {
        window.PollEvents();
        const auto acquired = swapchain.Acquire();
        if (acquired.outOfDate)
        {
            device.WaitIdle();
            swapchain.Resize(window.Extent());
            rebuildBackbuffers();
            continue;
        }

        VriCommandBuffer* cmd  = frames.Begin();
        auto&             slot = allocators[frames.FrameIndex()];
        slot.Reset();
        vrf::fg::RenderContext rc {device, cmd, samplers, slot};
        lastOut   = frames.FrameIndex();
        auto& out = outTex[lastOut];

        const float     seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        const float     angle   = seconds * 0.5f;
        const glm::vec3 eye     = center + glm::vec3 {std::cos(angle), 0.25f, std::sin(angle)} * (radius * 2.4f);
        const float     aspect  = static_cast<float>(kExtent.width) / static_cast<float>(kExtent.height);
        const glm::mat4 view    = glm::lookAt(eye, center, glm::vec3 {0, 1, 0});
        // NOTE: no proj[1][1] *= -1 here. That flip compensates the rasterizer's
        // inverted-Y framebuffer; this compute ray tracer builds rays straight from
        // invViewProj (no framebuffer flip), so applying it would render upside down.
        const glm::mat4 proj = glm::perspective(glm::radians(50.0f), aspect, 0.05f, 1000.0f);

        // Build the AS once (aabb_gen compute -> BLAS/TLAS build).
        if (!builtAs)
        {
            core.CmdSetPipelineLayout(cmd, genLayout->handle);
            core.CmdSetPipeline(cmd, genPipeline);
            rc.resourceSet[0][0] = vrf::fg::bindings::RawDescriptor {splatView, VriDescriptorType_StructuredBuffer};
            rc.resourceSet[0][1] = vrf::fg::bindings::RawDescriptor {aabbView, VriDescriptorType_StorageBuffer};
            rc.BindPipeline(vrf::fg::PassPipeline {genPipeline, genLayout, true});
            const GenParams gp {.count = splatCount};
            core.CmdSetConstants(cmd, 0, &gp, sizeof(gp));
            const VriDispatchDesc disp {.x = (splatCount + 63) / 64, .y = 1, .z = 1};
            core.CmdDispatch(cmd, &disp);
            const VriBufferBarrierDesc bb {
                .buffer = aabbBuffer,
                .before = {VriAccess_ShaderResourceStorageWrite, VriPipelineStage_ComputeShader},
                .after  = {VriAccess_AccelerationStructureRead, VriPipelineStage_AccelerationStructureBuild}};
            const VriBarrierGroupDesc bg {.buffers = &bb, .bufferNum = 1};
            core.CmdBarrier(cmd, &bg);
            blas->CmdBuild(cmd);
            tlas->CmdBuild(cmd);
            builtAs = true;
        }

        // Ray-query into the storage texture.
        rc.TransitionTexture(
            out,
            {VriAccess_ShaderResourceStorageWrite, VriLayout_ShaderResourceStorage, VriPipelineStage_ComputeShader});
        core.CmdSetPipelineLayout(cmd, rtLayout->handle);
        core.CmdSetPipeline(cmd, rtPipeline);
        rc.resourceSet[0][0] =
            vrf::fg::bindings::RawDescriptor {tlas->Descriptor(), VriDescriptorType_AccelerationStructure};
        rc.resourceSet[0][1] = vrf::fg::bindings::RawDescriptor {splatView, VriDescriptorType_StructuredBuffer};
        rc.resourceSet[0][2] = vrf::fg::bindings::StorageImage {&out};
        rc.BindPipeline(vrf::fg::PassPipeline {rtPipeline, rtLayout, true});
        CameraBlock cam {};
        cam.invViewProj = glm::inverse(proj * view);
        cam.cameraPos   = eye;
        cam.resolution  = glm::uvec2 {kExtent.width, kExtent.height};
        core.CmdSetConstants(cmd, 0, &cam, sizeof(cam));
        const VriDispatchDesc disp {.x = (kExtent.width + 7) / 8, .y = (kExtent.height + 7) / 8, .z = 1};
        core.CmdDispatch(cmd, &disp);

        // Present: sample the RT output onto the backbuffer.
        rc.TransitionTexture(out,
                             {VriAccess_ShaderResourceRead, VriLayout_ShaderResource, VriPipelineStage_FragmentShader});
        auto& backbuffer = backbuffers[acquired.index];
        rc.TransitionTexture(
            backbuffer,
            {VriAccess_ColorAttachmentWrite, VriLayout_ColorAttachment, VriPipelineStage_ColorAttachmentOutput});
        vrf::fg::FramebufferInfo fb;
        fb.area = kExtent;
        fb.colorAttachments.push_back(vrf::fg::AttachmentInfo {.target = &backbuffer});
        rc.framebufferInfo   = fb;
        rc.resourceSet[0][0] = vrf::fg::bindings::SampledImage {&out};
        rc.SetSampler(0, 1, samplers["linearClamp"]);
        rc.RenderFullScreenPostProcess(presentPipeline);

        rc.TransitionTexture(backbuffer, {VriAccess_None, VriLayout_Present, VriPipelineStage_AllCommands});
        frames.Submit();
        swapchain.Present();
        ++frameCount;
        if (autoExit && std::chrono::steady_clock::now() - start > std::chrono::seconds(5))
            break;
    }

    device.WaitIdle();

    if (shotPath)
    {
        if (const auto r = vrf::SaveTextureToPng(device, outTex[lastOut], shotPath); !r)
            std::fprintf(stderr, "[gaussian-rt] screenshot: %s\n", r.error().message.c_str());
        else
            std::printf("[gaussian-rt] screenshot -> %s\n", shotPath);
    }

    for (auto& [_, s] : samplers)
        core.DestroyDescriptor(s);
    core.DestroyDescriptor(splatView);
    core.DestroyDescriptor(aabbView);
    core.DestroyBuffer(splatBuffer);
    core.DestroyBuffer(aabbBuffer);
    core.DestroyPipeline(genPipeline);
    core.DestroyPipeline(rtPipeline);
    core.DestroyPipeline(presentPipeline.pipeline);
    core.DestroyPipelineLayout(genLayout->handle);
    core.DestroyPipelineLayout(rtLayout->handle);
    core.DestroyPipelineLayout(presentPipeline.layout->handle);

    std::printf("[gaussian-rt] frames: %llu, splats: %u\n", static_cast<unsigned long long>(frameCount), splatCount);
    return 0;
}
