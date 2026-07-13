// vrf rt_triangle - headless validation of the ray-tracing wrappers: a
// one-triangle BLAS + TLAS, a raygen/miss/closest-hit pipeline from a cooked
// .vshlib (SBT built by the RayTracingPipelineBuilder), one CmdTraceRays into a
// storage image, then a readback that asserts the center pixel actually hit.
//
// Exits 0 with a notice when the device reports no ray tracing (e.g. MoltenVK).
#include <cstdio>
#include <cstring>
#include <vector>

#include <vrf/fg/render_context.hpp>
#include <vrf/fg/texture.hpp>
#include <vrf/gpu/frame.hpp>
#include <vrf/gpu/raytracing.hpp>
#include <vrf/gpu/render_device.hpp>
#include <vrf/gpu/shader_library.hpp>

namespace
{
    const unsigned char g_rtTriangleVshlib[] = {
#include "rt_triangle.vshlib.h"
    };

    constexpr uint32_t kSize = 256;
} // namespace

int main()
{
    vrf::RenderDeviceDesc deviceDesc;
    deviceDesc.api             = vrf::GraphicsApi::Vulkan;
    deviceDesc.validation      = true;
    deviceDesc.enabledFeatures = VriFeature_RayTracing;
    auto deviceResult          = vrf::RenderDevice::Create(deviceDesc);
    if (!deviceResult)
    {
        std::fprintf(stderr, "[rt-triangle] device: %s\n", deviceResult.error().message.c_str());
        return 1;
    }
    vrf::RenderDevice& device = *deviceResult;

    if (device.Desc()->hasRayTracing == VRI_FALSE)
    {
        std::printf("[rt-triangle] device has no ray tracing support - skipped.\n");
        return 0;
    }
    const VriCoreInterface& core = device.Core();

    // ---- geometry: one triangle in a host-visible build-input buffer -------
    const float vertices[9] = {0.0f, 0.6f, 0.0f, 0.6f, -0.6f, 0.0f, -0.6f, -0.6f, 0.0f};

    VriBuffer*          vertexBuffer = nullptr;
    const VriBufferDesc vertexDesc {
        .size           = sizeof(vertices),
        .usage          = VriBufferUsage_AccelerationBuildInput,
        .memoryLocation = VriMemoryLocation_HostUpload,
    };
    if (!vrf::Succeeded(core.CreateBuffer(device.Handle(), &vertexDesc, &vertexBuffer)))
    {
        std::fprintf(stderr, "[rt-triangle] vertex buffer creation failed\n");
        return 1;
    }
    std::memcpy(core.MapBuffer(vertexBuffer, 0, sizeof(vertices)), vertices, sizeof(vertices));
    core.UnmapBuffer(vertexBuffer);

    // ---- acceleration structures ------------------------------------------
    auto blas = vrf::Blas::Create(device,
                                  {VriAsTrianglesDesc {
                                      .vertexBuffer = vertexBuffer,
                                      .vertexCount  = 3,
                                      .vertexStride = sizeof(float) * 3,
                                      .vertexFormat = VriFormat_RGB32_SFLOAT,
                                  }});
    if (!blas)
    {
        std::fprintf(stderr, "[rt-triangle] BLAS: %s\n", blas.error().message.c_str());
        return 1;
    }

    auto tlas = vrf::Tlas::Create(device, 1);
    if (!tlas)
    {
        std::fprintf(stderr, "[rt-triangle] TLAS: %s\n", tlas.error().message.c_str());
        return 1;
    }
    const vrf::AsInstance instance = vrf::AsInstance::Make(vrf::Mat4 {1.0f}, blas->DeviceAddress());
    tlas->SetInstances(&instance, 1);

    // ---- pipeline + SBT -----------------------------------------------------
    auto shadersResult = vrf::ShaderLibrary::LoadFromMemory(g_rtTriangleVshlib, sizeof(g_rtTriangleVshlib));
    if (!shadersResult)
    {
        std::fprintf(stderr, "[rt-triangle] shaders: %s\n", shadersResult.error().message.c_str());
        return 1;
    }
    vrf::ShaderLibrary shaders = std::move(*shadersResult);

    auto raygen = shaders.Resolve("rt", vrf::ShaderStage::RayGen, {});
    auto miss   = shaders.Resolve("rt", vrf::ShaderStage::Miss, {});
    auto chit   = shaders.Resolve("rt", vrf::ShaderStage::ClosestHit, {});
    if (!raygen || !miss || !chit)
    {
        std::fprintf(stderr, "[rt-triangle] shader resolve failed\n");
        return 1;
    }

    auto layoutInfo    = std::make_shared<vrf::fg::PipelineLayoutInfo>();
    layoutInfo->sets   = {vrf::fg::PipelineLayoutInfo::Set {
        0,
        {
            {.baseRegister   = 0,
             .descriptorNum  = 1,
             .descriptorType = VriDescriptorType_AccelerationStructure,
             .shaderStages   = VriShaderStage_RayGen},
            {.baseRegister   = 1,
             .descriptorNum  = 1,
             .descriptorType = VriDescriptorType_StorageTexture,
             .shaderStages   = VriShaderStage_RayGen},
        },
    }};
    {
        std::vector<VriDescriptorSetDesc> sets;
        sets.push_back({.registerSpace = 0,
                        .ranges        = layoutInfo->sets[0].ranges.data(),
                        .rangeNum      = static_cast<uint32_t>(layoutInfo->sets[0].ranges.size())});
        const VriPipelineLayoutDesc layoutDesc {
            .descriptorSets    = sets.data(),
            .descriptorSetNum  = static_cast<uint32_t>(sets.size()),
            .shaderStages      = VriShaderStage_RayGen | VriShaderStage_Miss | VriShaderStage_ClosestHit,
        };
        if (!vrf::Succeeded(core.CreatePipelineLayout(device.Handle(), &layoutDesc, &layoutInfo->handle)))
        {
            std::fprintf(stderr, "[rt-triangle] pipeline layout creation failed\n");
            return 1;
        }
    }

    const auto toShader = [](const vrf::ResolvedShader& r) {
        return vrf::RayTracingPipelineBuilder::Shader {r.spirv, r.spirvSize, r.entryPoint.c_str()};
    };
    auto pipeline = vrf::RayTracingPipelineBuilder {}
                        .SetPipelineLayout(layoutInfo)
                        .SetRayGen(toShader(*raygen))
                        .AddMiss(toShader(*miss))
                        .AddHitGroup(toShader(*chit))
                        .SetMaxRecursionDepth(1)
                        .Build(device);
    if (!pipeline)
    {
        std::fprintf(stderr, "[rt-triangle] pipeline: %s\n", pipeline.error().message.c_str());
        return 1;
    }

    // ---- output image + readback buffer -------------------------------------
    auto outputResult = vrf::fg::Texture::Create(device,
                                                 {
                                                     .extent = {kSize, kSize},
                                                     .format = VriFormat_RGBA8_UNORM,
                                                     .usage  = VriTextureUsage_ShaderResourceStorage |
                                                              VriTextureUsage_TransferSrc,
                                                 });
    if (!outputResult)
    {
        std::fprintf(stderr, "[rt-triangle] output image: %s\n", outputResult.error().message.c_str());
        return 1;
    }
    vrf::fg::Texture output = std::move(*outputResult);

    VriBuffer*          readback = nullptr;
    const VriBufferDesc readbackDesc {
        .size           = uint64_t {kSize} * kSize * 4,
        .usage          = VriBufferUsage_TransferDst,
        .memoryLocation = VriMemoryLocation_HostReadback,
    };
    core.CreateBuffer(device.Handle(), &readbackDesc, &readback);

    // ---- record: build AS -> trace -> readback ------------------------------
    auto frameResult = vrf::Frame::Create(device);
    if (!frameResult)
    {
        std::fprintf(stderr, "[rt-triangle] frame: %s\n", frameResult.error().message.c_str());
        return 1;
    }
    vrf::Frame frame = std::move(*frameResult);

    vrf::fg::Samplers            samplers;
    vrf::fg::DescriptorAllocator descriptors {device};

    VriCommandBuffer* cmd = frame.Begin();
    {
        vrf::fg::RenderContext rc {device, cmd, samplers, descriptors};

        blas->CmdBuild(cmd);
        tlas->CmdBuild(cmd);

        rc.TransitionTexture(output,
                             {VriAccess_ShaderResourceStorageWrite,
                              VriLayout_ShaderResourceStorage,
                              VriPipelineStage_RayTracingShader});

        core.CmdSetPipelineLayout(cmd, layoutInfo->handle);
        core.CmdSetPipeline(cmd, pipeline->Handle());
        rc.resourceSet[0][0] =
            vrf::fg::bindings::RawDescriptor {tlas->Descriptor(), VriDescriptorType_AccelerationStructure};
        rc.resourceSet[0][1] = vrf::fg::bindings::StorageImage {&output};
        // Reuse the context's descriptor-set path without rebinding the pipeline.
        rc.BindPipeline(vrf::fg::PassPipeline {pipeline->Handle(), layoutInfo, false});

        pipeline->CmdTraceRays(cmd, kSize, kSize);

        rc.TransitionTexture(output, {VriAccess_CopySourceRead, VriLayout_CopySource, VriPipelineStage_Transfer});
        const VriBufferTextureCopyDesc copy {
            .texture = {.layerNum = 1, .aspect = VriImageAspect_Color, .width = kSize, .height = kSize, .depth = 1},
        };
        core.CmdReadbackTextureToBuffer(cmd, readback, output.Handle(), &copy);
    }
    frame.Submit(); // synchronous: waits for the GPU

    // ---- verify --------------------------------------------------------------
    const auto* pixels = static_cast<const uint8_t*>(core.MapBuffer(readback, 0, readbackDesc.size));
    const auto  center = (uint64_t {kSize / 2} * kSize + kSize / 2) * 4;
    const auto  corner = uint64_t {0};
    const bool  centerHit  = pixels[center + 0] > 0 || pixels[center + 1] > 0; // barycentric shading
    const bool  cornerMiss = pixels[corner + 2] > pixels[corner + 0];          // dark blue miss color
    std::printf("[rt-triangle] center pixel: (%u, %u, %u) %s, corner pixel: (%u, %u, %u) %s\n",
                pixels[center + 0],
                pixels[center + 1],
                pixels[center + 2],
                centerHit ? "HIT" : "MISS?!",
                pixels[corner + 0],
                pixels[corner + 1],
                pixels[corner + 2],
                cornerMiss ? "miss (expected)" : "hit?!");
    core.UnmapBuffer(readback);

    // ---- cleanup ---------------------------------------------------------------
    device.WaitIdle();
    core.DestroyBuffer(readback);
    core.DestroyBuffer(vertexBuffer);
    core.DestroyPipelineLayout(layoutInfo->handle);

    const bool ok = centerHit && cornerMiss;
    std::printf("[rt-triangle] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
