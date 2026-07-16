// vrf rt_inpaint - the ray-traced-inpaint SHADING CORE, standalone. A synthetic
// triangle scene (ground plane + a few boxes) becomes a TRIANGLE BLAS/TLAS; a
// compute shader casts one primary ray per pixel, takes the closest committed
// triangle hit, derives the geometric normal, interpolates the per-vertex albedo,
// and shades it with a directional light. This is exactly what the
// pixelwise-viewpoint-warping RT inpaint does at each disocclusion hole - proven
// here on the native Metal backend (VRI_API=metal) before wiring into the warp
// pipeline, sidestepping pvw's full-pipeline Metal gaps.
//
// Runs on any backend with hasRayQuery - Vulkan (MoltenVK) and the native Metal
// backend. VRF_EXAMPLE_AUTO_EXIT runs a few seconds and quits;
// VRF_EXAMPLE_SCREENSHOT=<path.png> dumps the RT output via GPU readback.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

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
#include "rt_inpaint.vshlib.h"
    };

    struct CameraBlock
    {
        glm::mat4  invViewProj; // clip -> world
        glm::vec3  cameraPos;
        float      pad0;
        glm::uvec2 resolution;
        glm::uvec2 pad1;
        glm::vec3  lightDir; // world-space, points FROM the light
        float      pad2;
    };

    // --- synthetic triangle scene (non-indexed: 3 unique verts per triangle) -----
    struct Scene
    {
        std::vector<glm::vec4> vertices; // .xyz position (w unused)
        std::vector<glm::vec4> colors;   // .rgb albedo    (w unused)
    };

    void addTri(Scene& s, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& col)
    {
        s.vertices.push_back(glm::vec4 {a, 1.0f});
        s.vertices.push_back(glm::vec4 {b, 1.0f});
        s.vertices.push_back(glm::vec4 {c, 1.0f});
        for (int i = 0; i < 3; ++i)
            s.colors.push_back(glm::vec4 {col, 1.0f});
    }

    void addQuad(Scene&           s,
                 const glm::vec3& p0,
                 const glm::vec3& p1,
                 const glm::vec3& p2,
                 const glm::vec3& p3,
                 const glm::vec3& col)
    {
        addTri(s, p0, p1, p2, col);
        addTri(s, p0, p2, p3, col);
    }

    // Axis-aligned box as 12 triangles. Two-sided shading in the shader, so winding
    // is irrelevant here.
    void addBox(Scene& s, const glm::vec3& center, const glm::vec3& half, const glm::vec3& col)
    {
        glm::vec3 v[8];
        for (int i = 0; i < 8; ++i)
        {
            const float sx = (i & 1) ? 1.0f : -1.0f;
            const float sy = (i & 2) ? 1.0f : -1.0f;
            const float sz = (i & 4) ? 1.0f : -1.0f;
            v[i]           = center + half * glm::vec3 {sx, sy, sz};
        }
        // index = (x?1) | (y?2) | (z?4)
        addQuad(s, v[0], v[1], v[3], v[2], col); // -Z
        addQuad(s, v[4], v[6], v[7], v[5], col); // +Z
        addQuad(s, v[0], v[4], v[5], v[1], col); // -Y
        addQuad(s, v[2], v[3], v[7], v[6], col); // +Y
        addQuad(s, v[0], v[2], v[6], v[4], col); // -X
        addQuad(s, v[1], v[5], v[7], v[3], col); // +X
    }

    Scene makeScene()
    {
        Scene           s;
        constexpr float G = 6.0f; // ground half-size
        addQuad(s,
                glm::vec3 {-G, 0.0f, -G},
                glm::vec3 {G, 0.0f, -G},
                glm::vec3 {G, 0.0f, G},
                glm::vec3 {-G, 0.0f, G},
                glm::vec3 {0.55f, 0.55f, 0.58f});
        addBox(s, glm::vec3 {-1.5f, 0.60f, -0.5f}, glm::vec3 {0.6f}, glm::vec3 {0.85f, 0.22f, 0.20f}); // red
        addBox(s, glm::vec3 {1.2f, 1.00f, 0.8f}, glm::vec3 {1.0f}, glm::vec3 {0.25f, 0.42f, 0.85f});   // blue
        addBox(s, glm::vec3 {0.2f, 0.35f, 1.9f}, glm::vec3 {0.35f}, glm::vec3 {0.88f, 0.76f, 0.20f});  // yellow
        return s;
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

int main(int, char**)
{
    constexpr vrf::Extent2D kExtent {1280, 720};

    const Scene    scene       = makeScene();
    const uint32_t vertexCount = static_cast<uint32_t>(scene.vertices.size());
    const uint32_t triCount    = vertexCount / 3;
    std::printf("[rt-inpaint] synthetic scene: %u triangles (%u verts)\n", triCount, vertexCount);

    // ---- window / device / swapchain / frames -----------------------------
    vrf::WindowDesc windowDesc;
    windowDesc.title  = "vrf rt_inpaint";
    windowDesc.extent = kExtent;
    auto windowResult = vrf::Window::Create(windowDesc);
    if (!windowResult)
    {
        std::fprintf(stderr, "[rt-inpaint] window: %s\n", windowResult.error().message.c_str());
        return 1;
    }
    auto& window = *windowResult.value();

    vrf::RenderDeviceDesc deviceDesc;
    deviceDesc.validation      = true;
    deviceDesc.enabledFeatures = VriFeature_RayQuery;
    // Native Metal backend (inline ray query) via VRI_API=metal; default Vulkan/MoltenVK.
    if (const char* api = std::getenv("VRI_API"))
        if (std::string_view {api} == "metal")
            deviceDesc.api = vrf::GraphicsApi::Metal;
    auto deviceResult = vrf::RenderDevice::Create(deviceDesc);
    if (!deviceResult)
    {
        std::fprintf(stderr, "[rt-inpaint] device: %s\n", deviceResult.error().message.c_str());
        return 1;
    }
    vrf::RenderDevice&      device = *deviceResult;
    const VriCoreInterface& core   = device.Core();

    if (device.Desc()->hasRayQuery == VRI_FALSE)
    {
        std::printf("[rt-inpaint] device has no ray query support - skipped.\n");
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

    // ---- scene buffers: vertices (BLAS input + shader), colors (shader) ----
    VriBuffer* vertexBuffer = makeBuffer(device,
                                         uint64_t {vertexCount} * sizeof(glm::vec4),
                                         VriBufferUsage_StorageBuffer | VriBufferUsage_AccelerationBuildInput,
                                         sizeof(glm::vec4),
                                         VriMemoryLocation_HostUpload,
                                         scene.vertices.data());
    VriBuffer* colorBuffer  = makeBuffer(device,
                                        uint64_t {vertexCount} * sizeof(glm::vec4),
                                        VriBufferUsage_StorageBuffer,
                                        sizeof(glm::vec4),
                                        VriMemoryLocation_HostUpload,
                                        scene.colors.data());
    if (!vertexBuffer || !colorBuffer)
    {
        std::fprintf(stderr, "[rt-inpaint] scene buffers failed\n");
        return 1;
    }
    VriDescriptor* vertexView = nullptr;
    VriDescriptor* colorView  = nullptr;
    {
        const VriBufferViewDesc vv {.buffer = vertexBuffer, .viewType = VriDescriptorType_StructuredBuffer};
        core.CreateBufferView(device.Handle(), &vv, &vertexView);
        const VriBufferViewDesc cv {.buffer = colorBuffer, .viewType = VriDescriptorType_StructuredBuffer};
        core.CreateBufferView(device.Handle(), &cv, &colorView);
    }

    // ---- pipelines: rt_inpaint (compute), present -------------------------
    using Set     = vrf::fg::PipelineLayoutInfo::Set;
    auto rtLayout = [&] {
        auto info  = std::make_shared<vrf::fg::PipelineLayoutInfo>();
        info->sets = {Set {0,
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
                             .descriptorType = VriDescriptorType_StructuredBuffer,
                             .shaderStages   = VriShaderStage_Compute},
                            {.baseRegister   = 3,
                             .descriptorNum  = 1,
                             .descriptorType = VriDescriptorType_StorageTexture,
                             .shaderStages   = VriShaderStage_Compute}}}};
        std::vector<VriDescriptorSetDesc> sets;
        sets.push_back({.registerSpace = 0,
                        .ranges        = info->sets[0].ranges.data(),
                        .rangeNum      = static_cast<uint32_t>(info->sets[0].ranges.size())});
        const VriPushConstantDesc push {
            .baseRegister = 0, .size = sizeof(CameraBlock), .shaderStages = VriShaderStage_Compute};
        const VriPipelineLayoutDesc ld {.descriptorSets   = sets.data(),
                                        .descriptorSetNum = 1,
                                        .pushConstants    = &push,
                                        .pushConstantNum  = 1,
                                        .shaderStages     = VriShaderStage_Compute};
        core.CreatePipelineLayout(device.Handle(), &ld, &info->handle);
        return info;
    }();
    VriPipeline* rtPipeline = [&] {
        auto cs = shaders.Resolve("rt_inpaint", vrf::ShaderStage::Compute, {});
        if (!cs)
        {
            std::fprintf(stderr, "[rt-inpaint] shader 'rt_inpaint' resolve failed\n");
            std::exit(1);
        }
        auto p = vrf::ComputePipelineBuilder {}
                     .SetPipelineLayout(rtLayout->handle)
                     .SetShader(cs->spirv, cs->spirvSize, cs->entryPoint.c_str())
                     .Build(device);
        if (!p)
        {
            std::fprintf(stderr, "[rt-inpaint] compute pipeline: %s\n", p.error().message.c_str());
            std::exit(1);
        }
        return *p;
    }();

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

    // ---- triangle BLAS/TLAS (built once - static geometry) ----------------
    auto blas = vrf::Blas::Create(device,
                                  {VriAsTrianglesDesc {.vertexBuffer = vertexBuffer,
                                                       .vertexCount  = vertexCount,
                                                       .vertexStride = sizeof(glm::vec4),
                                                       .vertexFormat = VriFormat_RGB32_SFLOAT}});
    if (!blas)
    {
        std::fprintf(stderr, "[rt-inpaint] BLAS: %s\n", blas.error().message.c_str());
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

    const glm::vec3 center   = glm::vec3 {0.0f, 0.7f, 0.4f};
    const float     radius   = 3.6f;
    const glm::vec3 lightDir = glm::normalize(glm::vec3 {-0.5f, -1.0f, -0.35f});

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
        const glm::vec3 eye     = center + glm::vec3 {std::cos(angle), 0.55f, std::sin(angle)} * radius;
        const float     aspect  = static_cast<float>(kExtent.width) / static_cast<float>(kExtent.height);
        const glm::mat4 view    = glm::lookAt(eye, center, glm::vec3 {0, 1, 0});
        // No proj[1][1] *= -1: this compute ray tracer builds rays from invViewProj
        // with no framebuffer flip (applying it would render upside down).
        const glm::mat4 proj = glm::perspective(glm::radians(55.0f), aspect, 0.05f, 1000.0f);

        // Build the AS once (static triangle geometry).
        if (!builtAs)
        {
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
        rc.resourceSet[0][1] = vrf::fg::bindings::RawDescriptor {vertexView, VriDescriptorType_StructuredBuffer};
        rc.resourceSet[0][2] = vrf::fg::bindings::RawDescriptor {colorView, VriDescriptorType_StructuredBuffer};
        rc.resourceSet[0][3] = vrf::fg::bindings::StorageImage {&out};
        rc.BindPipeline(vrf::fg::PassPipeline {rtPipeline, rtLayout, true});
        CameraBlock cam {};
        cam.invViewProj = glm::inverse(proj * view);
        cam.cameraPos   = eye;
        cam.resolution  = glm::uvec2 {kExtent.width, kExtent.height};
        cam.lightDir    = lightDir;
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
            std::fprintf(stderr, "[rt-inpaint] screenshot: %s\n", r.error().message.c_str());
        else
            std::printf("[rt-inpaint] screenshot -> %s\n", shotPath);
    }

    std::printf("[rt-inpaint] frames: %llu, triangles: %u\n", static_cast<unsigned long long>(frameCount), triCount);
    return 0;
}
