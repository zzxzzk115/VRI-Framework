// vrf compute_rayquery - headless validation of inline ray tracing (RayQuery):
// a one-triangle BLAS + TLAS traced by a compute shader (no RT pipeline / SBT),
// then a readback that asserts the center ray hit and a corner ray missed.
//
// This is the inline counterpart to rt_triangle. It gates on hasRayQuery (not
// hasRayTracing), so it runs on backends without a DXR-style SBT - notably the
// native Metal backend, which reports hasRayQuery via MTL ray queries. This is
// the path a Metal 3D Gaussian ray tracer (3DGRT) builds on.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <vrf/fg/render_context.hpp>
#include <vrf/gpu/builders/compute_pipeline_builder.hpp>
#include <vrf/gpu/frame.hpp>
#include <vrf/gpu/raytracing.hpp>
#include <vrf/gpu/render_device.hpp>
#include <vrf/gpu/shader_library.hpp>

namespace
{
    const unsigned char g_vshlib[] = {
#include "rayquery.vshlib.h"
    };

    constexpr uint32_t kGrid = 32;

    struct Params
    {
        uint32_t grid;
    };
} // namespace

int main()
{
    vrf::RenderDeviceDesc deviceDesc;
    deviceDesc.validation      = true;
    deviceDesc.enabledFeatures = VriFeature_RayQuery;
    auto deviceResult          = vrf::RenderDevice::Create(deviceDesc);
    if (!deviceResult)
    {
        std::fprintf(stderr, "[compute-rayquery] device: %s\n", deviceResult.error().message.c_str());
        return 1;
    }
    vrf::RenderDevice&      device = *deviceResult;
    const VriCoreInterface& core   = device.Core();

    if (device.Desc()->hasRayQuery == VRI_FALSE)
    {
        std::printf("[compute-rayquery] device has no ray query support - skipped.\n");
        return 0;
    }

    // ---- one triangle in a build-input buffer -----------------------------
    const float         vertices[9]  = {0.0f, 0.6f, 0.0f, 0.6f, -0.6f, 0.0f, -0.6f, -0.6f, 0.0f};
    VriBuffer*          vertexBuffer = nullptr;
    const VriBufferDesc vertexDesc {.size           = sizeof(vertices),
                                    .usage          = VriBufferUsage_AccelerationBuildInput,
                                    .memoryLocation = VriMemoryLocation_HostUpload};
    if (!vrf::Succeeded(core.CreateBuffer(device.Handle(), &vertexDesc, &vertexBuffer)))
    {
        std::fprintf(stderr, "[compute-rayquery] vertex buffer creation failed\n");
        return 1;
    }
    std::memcpy(core.MapBuffer(vertexBuffer, 0, sizeof(vertices)), vertices, sizeof(vertices));
    core.UnmapBuffer(vertexBuffer);

    auto blas = vrf::Blas::Create(device,
                                  {VriAsTrianglesDesc {
                                      .vertexBuffer = vertexBuffer,
                                      .vertexCount  = 3,
                                      .vertexStride = sizeof(float) * 3,
                                      .vertexFormat = VriFormat_RGB32_SFLOAT,
                                  }});
    if (!blas)
    {
        std::fprintf(stderr, "[compute-rayquery] BLAS: %s\n", blas.error().message.c_str());
        return 1;
    }
    auto tlas = vrf::Tlas::Create(device, 1);
    if (!tlas)
    {
        std::fprintf(stderr, "[compute-rayquery] TLAS: %s\n", tlas.error().message.c_str());
        return 1;
    }
    const vrf::AsInstance instance = vrf::AsInstance::Make(vrf::Mat4 {1.0f}, blas->DeviceAddress());
    tlas->SetInstances(&instance, 1);

    // ---- pipeline layout: set 0 { TLAS, RW out } + push constant ----------
    auto layoutInfo  = std::make_shared<vrf::fg::PipelineLayoutInfo>();
    layoutInfo->sets = {vrf::fg::PipelineLayoutInfo::Set {
        0,
        {
            {.baseRegister   = 0,
             .descriptorNum  = 1,
             .descriptorType = VriDescriptorType_AccelerationStructure,
             .shaderStages   = VriShaderStage_Compute},
            {.baseRegister   = 1,
             .descriptorNum  = 1,
             .descriptorType = VriDescriptorType_StorageBuffer,
             .shaderStages   = VriShaderStage_Compute},
        },
    }};
    {
        std::vector<VriDescriptorSetDesc> sets;
        sets.push_back({.registerSpace = 0,
                        .ranges        = layoutInfo->sets[0].ranges.data(),
                        .rangeNum      = static_cast<uint32_t>(layoutInfo->sets[0].ranges.size())});
        const VriPushConstantDesc push {
            .baseRegister = 0, .size = sizeof(Params), .shaderStages = VriShaderStage_Compute};
        const VriPipelineLayoutDesc layoutDesc {.descriptorSets   = sets.data(),
                                                .descriptorSetNum = 1,
                                                .pushConstants    = &push,
                                                .pushConstantNum  = 1,
                                                .shaderStages     = VriShaderStage_Compute};
        if (!vrf::Succeeded(core.CreatePipelineLayout(device.Handle(), &layoutDesc, &layoutInfo->handle)))
        {
            std::fprintf(stderr, "[compute-rayquery] pipeline layout creation failed\n");
            return 1;
        }
    }

    auto shadersResult = vrf::ShaderLibrary::LoadFromMemory(g_vshlib, sizeof(g_vshlib));
    if (!shadersResult)
    {
        std::fprintf(stderr, "[compute-rayquery] shaders: %s\n", shadersResult.error().message.c_str());
        return 1;
    }
    vrf::ShaderLibrary shaders = std::move(*shadersResult);
    auto               cs      = shaders.Resolve("rayquery", vrf::ShaderStage::Compute, {});
    if (!cs)
    {
        std::fprintf(stderr, "[compute-rayquery] compute shader resolve failed\n");
        return 1;
    }
    auto pipeline = vrf::ComputePipelineBuilder {}
                        .SetPipelineLayout(layoutInfo->handle)
                        .SetShader(cs->spirv, cs->spirvSize, cs->entryPoint.c_str())
                        .Build(device);
    if (!pipeline)
    {
        std::fprintf(stderr, "[compute-rayquery] pipeline: %s\n", pipeline.error().message.c_str());
        return 1;
    }

    // ---- output + readback -------------------------------------------------
    const uint64_t      outSize   = uint64_t {kGrid} * kGrid * sizeof(float);
    VriBuffer*          outBuffer = nullptr;
    const VriBufferDesc outDesc {.size           = outSize,
                                 .usage          = VriBufferUsage_StorageBuffer | VriBufferUsage_TransferSrc,
                                 .memoryLocation = VriMemoryLocation_Device};
    core.CreateBuffer(device.Handle(), &outDesc, &outBuffer);
    VriDescriptor*          outView = nullptr;
    const VriBufferViewDesc ovd {.buffer = outBuffer, .viewType = VriDescriptorType_StorageBuffer};
    core.CreateBufferView(device.Handle(), &ovd, &outView);

    VriBuffer*          readback = nullptr;
    const VriBufferDesc rbDesc {
        .size = outSize, .usage = VriBufferUsage_TransferDst, .memoryLocation = VriMemoryLocation_HostReadback};
    core.CreateBuffer(device.Handle(), &rbDesc, &readback);

    // ---- record: build AS -> ray-query -> readback ------------------------
    auto frameResult = vrf::Frame::Create(device);
    if (!frameResult)
    {
        std::fprintf(stderr, "[compute-rayquery] frame: %s\n", frameResult.error().message.c_str());
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

        core.CmdSetPipelineLayout(cmd, layoutInfo->handle);
        core.CmdSetPipeline(cmd, *pipeline);
        rc.resourceSet[0][0] =
            vrf::fg::bindings::RawDescriptor {tlas->Descriptor(), VriDescriptorType_AccelerationStructure};
        rc.resourceSet[0][1] = vrf::fg::bindings::RawDescriptor {outView, VriDescriptorType_StorageBuffer};
        rc.BindPipeline(vrf::fg::PassPipeline {*pipeline, layoutInfo, true});

        const Params params {.grid = kGrid};
        core.CmdSetConstants(cmd, 0, &params, sizeof(params));
        const VriDispatchDesc dispatch {.x = (kGrid + 7) / 8, .y = (kGrid + 7) / 8, .z = 1};
        core.CmdDispatch(cmd, &dispatch);

        const VriBufferBarrierDesc bb {.buffer = outBuffer,
                                       .before = {VriAccess_ShaderResourceStorageWrite, VriPipelineStage_ComputeShader},
                                       .after  = {VriAccess_CopySourceRead, VriPipelineStage_Transfer}};
        const VriBarrierGroupDesc  bg {.buffers = &bb, .bufferNum = 1};
        core.CmdBarrier(cmd, &bg);
        const VriBufferCopyDesc copy {.dstOffset = 0, .srcOffset = 0, .size = outSize};
        core.CmdCopyBuffer(cmd, readback, outBuffer, &copy);
    }
    frame.Submit();

    // ---- verify: center hit, corner miss ----------------------------------
    const auto* dist       = static_cast<const float*>(core.MapBuffer(readback, 0, outSize));
    const float centerDist = dist[(kGrid / 2) * kGrid + kGrid / 2];
    const float cornerDist = dist[0];
    const bool  centerHit  = centerDist > 0.0f;
    const bool  cornerMiss = cornerDist < 0.0f;
    std::printf("[compute-rayquery] center ray t=%.3f %s, corner ray t=%.3f %s\n",
                centerDist,
                centerHit ? "HIT" : "MISS?!",
                cornerDist,
                cornerMiss ? "miss (expected)" : "hit?!");
    core.UnmapBuffer(readback);

    device.WaitIdle();
    core.DestroyBuffer(readback);
    core.DestroyDescriptor(outView);
    core.DestroyBuffer(outBuffer);
    core.DestroyBuffer(vertexBuffer);
    core.DestroyPipeline(*pipeline);
    core.DestroyPipelineLayout(layoutInfo->handle);

    const bool ok = centerHit && cornerMiss;
    std::printf("[compute-rayquery] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
