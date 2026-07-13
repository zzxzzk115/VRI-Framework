#include "vrf/fg/framegraph_buffer.hpp"

#include <cassert>
#include <format>

#include "vrf/fg/render_context.hpp"
#include "vrf/fg/transient_resources.hpp"

namespace vrf::fg
{
    void FrameGraphBuffer::create(const Desc& desc, void* allocator)
    {
        buffer = static_cast<TransientResources*>(allocator)->acquireBuffer(desc);
    }

    void FrameGraphBuffer::destroy(const Desc& desc, void* allocator)
    {
        static_cast<TransientResources*>(allocator)->releaseBuffer(desc, buffer);
        buffer = nullptr;
    }

    void FrameGraphBuffer::preRead(const Desc& desc, const uint32_t flags, void* ctx)
    {
        auto& rc = *static_cast<RenderContext*>(ctx);

        const auto bindingInfo = decodeBindingInfo(flags);

        VriAccessStage after {};
        if (static_cast<bool>(bindingInfo.pipelineStage & PipelineStage::Transfer))
        {
            after = {VriAccess_CopySourceRead, VriPipelineStage_Transfer};
        }
        else
        {
            switch (desc.type)
            {
                case BufferType::IndexBuffer:
                    after = {VriAccess_IndexBufferRead, VriPipelineStage_VertexInput};
                    break;
                case BufferType::VertexBuffer:
                    after = {VriAccess_VertexBufferRead, VriPipelineStage_VertexInput};
                    break;
                case BufferType::UniformBuffer:
                    after = {VriAccess_ConstantBufferRead, convert(bindingInfo.pipelineStage)};
                    break;
                case BufferType::StorageBuffer:
                    after = {VriAccess_ShaderResourceStorageRead, convert(bindingInfo.pipelineStage)};
                    break;
            }

            const auto [set, binding] = bindingInfo.location;
            switch (desc.type)
            {
                case BufferType::UniformBuffer:
                    rc.resourceSet[set][binding] = bindings::UniformBuffer {buffer};
                    break;
                case BufferType::StorageBuffer:
                    rc.resourceSet[set][binding] = bindings::StorageBuffer {buffer};
                    break;
                default:
                    break;
            }
        }
        rc.TransitionBuffer(*buffer, after);
    }

    void FrameGraphBuffer::preWrite(const Desc& desc, const uint32_t flags, void* ctx)
    {
        auto& rc = *static_cast<RenderContext*>(ctx);

        const auto [location, pipelineStage] = decodeBindingInfo(flags);

        VriAccessStage after {};
        if (static_cast<bool>(pipelineStage & PipelineStage::Transfer))
        {
            after = {VriAccess_CopyDestinationWrite, VriPipelineStage_Transfer};
        }
        else
        {
            assert(desc.type == BufferType::StorageBuffer);
            after = {VriAccess_ShaderResourceStorageRead | VriAccess_ShaderResourceStorageWrite,
                     convert(pipelineStage)};

            const auto [set, binding]    = location;
            rc.resourceSet[set][binding] = bindings::StorageBuffer {buffer};
        }
        rc.TransitionBuffer(*buffer, after);
    }

    std::string FrameGraphBuffer::toString(const Desc& desc) { return std::format("size: {}", desc.dataSize()); }
} // namespace vrf::fg
