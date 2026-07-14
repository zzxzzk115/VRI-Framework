// vrf compute_saxpy - headless validation of the ComputePipelineBuilder: a
// SAXPY (y = a*x + y) compute shader from a cooked .vshlib, dispatched over two
// storage buffers, then a readback that asserts every element transformed.
//
// This is the compute counterpart to rt_triangle: it proves a compute pipeline
// built by ComputePipelineBuilder binds storage buffers, takes a push constant,
// dispatches, and writes device memory end to end.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <vrf/fg/render_context.hpp>
#include <vrf/gpu/builders/compute_pipeline_builder.hpp>
#include <vrf/gpu/frame.hpp>
#include <vrf/gpu/render_device.hpp>
#include <vrf/gpu/shader_library.hpp>

namespace
{
    const unsigned char g_saxpyVshlib[] = {
#include "saxpy.vshlib.h"
    };

    constexpr uint32_t kCount = 4096;

    struct Params
    {
        float    a;
        uint32_t count;
    };

    // Host-visible storage buffer of `count` floats, filled from `init`.
    VriBuffer* makeStorageBuffer(const vrf::RenderDevice& device, const float* init, uint32_t count)
    {
        const VriCoreInterface& core   = device.Core();
        VriBuffer*              buffer = nullptr;
        const VriBufferDesc     desc {
                .size            = uint64_t {count} * sizeof(float),
                .structureStride = sizeof(float),
                .usage           = VriBufferUsage_StorageBuffer | VriBufferUsage_TransferSrc,
                .memoryLocation  = VriMemoryLocation_HostUpload,
        };
        if (!vrf::Succeeded(core.CreateBuffer(device.Handle(), &desc, &buffer)))
            return nullptr;
        std::memcpy(core.MapBuffer(buffer, 0, desc.size), init, desc.size);
        core.UnmapBuffer(buffer);
        return buffer;
    }
} // namespace

int main()
{
    vrf::RenderDeviceDesc deviceDesc;
    deviceDesc.api        = vrf::GraphicsApi::Vulkan;
    deviceDesc.validation = true;
    auto deviceResult     = vrf::RenderDevice::Create(deviceDesc);
    if (!deviceResult)
    {
        std::fprintf(stderr, "[compute-saxpy] device: %s\n", deviceResult.error().message.c_str());
        return 1;
    }
    vrf::RenderDevice&      device = *deviceResult;
    const VriCoreInterface& core   = device.Core();

    // ---- input data: x[i] = i, y[i] = 1000 -> expect a*i + 1000 -----------
    constexpr float    kA = 2.0f;
    std::vector<float> x(kCount), y(kCount);
    for (uint32_t i = 0; i < kCount; ++i)
    {
        x[i] = static_cast<float>(i);
        y[i] = 1000.0f;
    }

    VriBuffer* xBuffer = makeStorageBuffer(device, x.data(), kCount);
    VriBuffer* yBuffer = makeStorageBuffer(device, y.data(), kCount);
    if (!xBuffer || !yBuffer)
    {
        std::fprintf(stderr, "[compute-saxpy] storage buffer creation failed\n");
        return 1;
    }

    VriDescriptor*          xView = nullptr;
    VriDescriptor*          yView = nullptr;
    const VriBufferViewDesc xViewDesc {.buffer = xBuffer, .viewType = VriDescriptorType_StructuredBuffer};
    const VriBufferViewDesc yViewDesc {.buffer = yBuffer, .viewType = VriDescriptorType_StorageBuffer};
    core.CreateBufferView(device.Handle(), &xViewDesc, &xView);
    core.CreateBufferView(device.Handle(), &yViewDesc, &yView);

    // ---- pipeline layout: set 0 { x SRV, y UAV } + push constant ----------
    auto layoutInfo  = std::make_shared<vrf::fg::PipelineLayoutInfo>();
    layoutInfo->sets = {vrf::fg::PipelineLayoutInfo::Set {
        0,
        {
            {.baseRegister   = 0,
             .descriptorNum  = 1,
             .descriptorType = VriDescriptorType_StructuredBuffer,
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
        const VriPipelineLayoutDesc layoutDesc {
            .descriptorSets   = sets.data(),
            .descriptorSetNum = static_cast<uint32_t>(sets.size()),
            .pushConstants    = &push,
            .pushConstantNum  = 1,
            .shaderStages     = VriShaderStage_Compute,
        };
        if (!vrf::Succeeded(core.CreatePipelineLayout(device.Handle(), &layoutDesc, &layoutInfo->handle)))
        {
            std::fprintf(stderr, "[compute-saxpy] pipeline layout creation failed\n");
            return 1;
        }
    }

    // ---- compute pipeline via the new builder -----------------------------
    auto shadersResult = vrf::ShaderLibrary::LoadFromMemory(g_saxpyVshlib, sizeof(g_saxpyVshlib));
    if (!shadersResult)
    {
        std::fprintf(stderr, "[compute-saxpy] shaders: %s\n", shadersResult.error().message.c_str());
        return 1;
    }
    vrf::ShaderLibrary shaders = std::move(*shadersResult);
    auto               cs      = shaders.Resolve("saxpy", vrf::ShaderStage::Compute, {});
    if (!cs)
    {
        std::fprintf(stderr, "[compute-saxpy] compute shader resolve failed\n");
        return 1;
    }

    auto pipeline = vrf::ComputePipelineBuilder {}
                        .SetPipelineLayout(layoutInfo->handle)
                        .SetShader(cs->spirv, cs->spirvSize, cs->entryPoint.c_str())
                        .Build(device);
    if (!pipeline)
    {
        std::fprintf(stderr, "[compute-saxpy] pipeline: %s\n", pipeline.error().message.c_str());
        return 1;
    }

    // ---- readback buffer ---------------------------------------------------
    VriBuffer*          readback = nullptr;
    const VriBufferDesc readbackDesc {
        .size           = uint64_t {kCount} * sizeof(float),
        .usage          = VriBufferUsage_TransferDst,
        .memoryLocation = VriMemoryLocation_HostReadback,
    };
    core.CreateBuffer(device.Handle(), &readbackDesc, &readback);

    // ---- record: dispatch -> copy y to readback ----------------------------
    auto frameResult = vrf::Frame::Create(device);
    if (!frameResult)
    {
        std::fprintf(stderr, "[compute-saxpy] frame: %s\n", frameResult.error().message.c_str());
        return 1;
    }
    vrf::Frame frame = std::move(*frameResult);

    vrf::fg::Samplers            samplers;
    vrf::fg::DescriptorAllocator descriptors {device};

    VriCommandBuffer* cmd = frame.Begin();
    {
        vrf::fg::RenderContext rc {device, cmd, samplers, descriptors};

        core.CmdSetPipelineLayout(cmd, layoutInfo->handle);
        core.CmdSetPipeline(cmd, *pipeline);
        rc.resourceSet[0][0] = vrf::fg::bindings::RawDescriptor {xView, VriDescriptorType_StructuredBuffer};
        rc.resourceSet[0][1] = vrf::fg::bindings::RawDescriptor {yView, VriDescriptorType_StorageBuffer};
        rc.BindPipeline(vrf::fg::PassPipeline {*pipeline, layoutInfo, true});

        const Params params {.a = kA, .count = kCount};
        core.CmdSetConstants(cmd, 0, &params, sizeof(params));

        const VriDispatchDesc dispatch {.x = (kCount + 63) / 64, .y = 1, .z = 1};
        core.CmdDispatch(cmd, &dispatch);

        // storage-write -> transfer-read barrier before the copy.
        const VriBufferBarrierDesc barrier {
            .buffer = yBuffer,
            .before = {VriAccess_ShaderResourceStorageWrite, VriPipelineStage_ComputeShader},
            .after  = {VriAccess_CopySourceRead, VriPipelineStage_Transfer},
        };
        const VriBarrierGroupDesc barriers {.buffers = &barrier, .bufferNum = 1};
        core.CmdBarrier(cmd, &barriers);

        const VriBufferCopyDesc copy {.dstOffset = 0, .srcOffset = 0, .size = readbackDesc.size};
        core.CmdCopyBuffer(cmd, readback, yBuffer, &copy);
    }
    frame.Submit(); // synchronous: waits for the GPU

    // ---- verify: y[i] == a*i + 1000 ----------------------------------------
    const auto* result     = static_cast<const float*>(core.MapBuffer(readback, 0, readbackDesc.size));
    uint32_t    mismatches = 0;
    for (uint32_t i = 0; i < kCount; ++i)
    {
        const float expected = kA * static_cast<float>(i) + 1000.0f;
        if (result[i] != expected)
        {
            if (mismatches < 4)
                std::fprintf(stderr, "[compute-saxpy] y[%u] = %f, expected %f\n", i, result[i], expected);
            ++mismatches;
        }
    }
    std::printf("[compute-saxpy] y[0]=%.1f y[1]=%.1f y[%u]=%.1f (a=%.1f)\n",
                result[0],
                result[1],
                kCount - 1,
                result[kCount - 1],
                kA);
    core.UnmapBuffer(readback);

    // ---- cleanup -----------------------------------------------------------
    device.WaitIdle();
    core.DestroyBuffer(readback);
    core.DestroyDescriptor(xView);
    core.DestroyDescriptor(yView);
    core.DestroyBuffer(xBuffer);
    core.DestroyBuffer(yBuffer);
    core.DestroyPipeline(*pipeline);
    core.DestroyPipelineLayout(layoutInfo->handle);

    const bool ok = mismatches == 0;
    std::printf("[compute-saxpy] %s (%u / %u correct)\n", ok ? "PASS" : "FAIL", kCount - mismatches, kCount);
    return ok ? 0 : 1;
}
