#include "vrf/gpu/builders/compute_pipeline_builder.hpp"

#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    Expected<VriPipeline*> ComputePipelineBuilder::Build(RenderDevice& device) const
    {
        const void* code = m_bytecode;
        size_t      size = m_size;

        if (m_hasVariants)
        {
            switch (device.Api())
            {
                case VriGraphicsAPI_WebGPU:
                    code = m_variants.wgsl;
                    size = m_variants.wgslSize;
                    break;
                case VriGraphicsAPI_D3D12:
                    code = m_variants.d3d12;
                    size = m_variants.d3d12Size;
                    break;
                default: // Vulkan / OpenGL / OpenGL ES / Metal consume SPIR-V (transpiled where needed)
                    code = m_variants.spirv;
                    size = m_variants.spirvSize;
                    break;
            }
        }

        if (!code || size == 0)
            return MakeError(std::string("ComputePipelineBuilder::Build: no shader blob for backend ") +
                             device.ApiName());

        VriComputePipelineDesc desc {};
        desc.pipelineLayout        = m_layout;
        desc.shader.stage          = VriShaderStage_Compute;
        desc.shader.bytecode       = code;
        desc.shader.bytecodeSize   = size;
        desc.shader.entryPointName = m_entryPoint.c_str();

        VriPipeline*    pipeline = nullptr;
        const VriResult r        = device.Core().CreateComputePipeline(device.Handle(), &desc, &pipeline);
        if (r != VriResult_Success)
            return MakeError(r, "ComputePipelineBuilder::Build", "CreateComputePipeline failed");
        return pipeline;
    }
} // namespace vrf
