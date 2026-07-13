#include "vrf/gpu/builders/graphics_pipeline_builder.hpp"

#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    GraphicsPipelineBuilder::GraphicsPipelineBuilder()
    {
        m_inputAssembly.topology         = VriPrimitiveTopology_TriangleList;
        m_inputAssembly.primitiveRestart = VRI_FALSE;

        m_rasterization.polygonMode        = VriPolygonMode_Fill;
        m_rasterization.cullMode           = VriCullMode_None;
        m_rasterization.frontFace          = VriFrontFace_CounterClockwise;
        m_rasterization.depthClamp         = VRI_FALSE;
        m_rasterization.lineWidth          = 1.0f;
        m_rasterization.conservativeRaster = VRI_FALSE;

        m_multisample.sampleNum = 1;
    }

    GraphicsPipelineBuilder& GraphicsPipelineBuilder::AddShader(VriShaderStageBits stage,
                                                                const void*        bytecode,
                                                                size_t             size,
                                                                const char*        entryPoint)
    {
        VriShaderDesc shader {};
        shader.stage          = stage;
        shader.bytecode       = bytecode;
        shader.bytecodeSize   = size;
        shader.entryPointName = entryPoint;
        m_shaders.push_back(shader);
        return *this;
    }

    GraphicsPipelineBuilder& GraphicsPipelineBuilder::AddShaderVariants(VriShaderStageBits    stage,
                                                                        const ShaderVariants& variants,
                                                                        std::string           entryPoint)
    {
        m_variantShaders.push_back(VariantShader {stage, std::move(entryPoint), variants});
        return *this;
    }

    GraphicsPipelineBuilder&
    GraphicsPipelineBuilder::AddColorAttachment(VriFormat format, VriColorWriteFlags writeMask, VriBlendDesc blend)
    {
        VriColorAttachmentDesc attachment {};
        attachment.format         = format;
        attachment.blend          = blend;
        attachment.colorWriteMask = writeMask;
        m_colorAttachments.push_back(attachment);
        return *this;
    }

    Expected<VriPipeline*> GraphicsPipelineBuilder::Build(RenderDevice& device) const
    {
        // Start with the raw shaders, then resolve each variant set to the active backend's blob.
        std::vector<VriShaderDesc> shaders = m_shaders;
        for (const VariantShader& vs : m_variantShaders)
        {
            const void* code = nullptr;
            size_t      size = 0;
            switch (device.Api())
            {
                case VriGraphicsAPI_WebGPU:
                    code = vs.variants.wgsl;
                    size = vs.variants.wgslSize;
                    break;
                case VriGraphicsAPI_D3D12:
                    code = vs.variants.d3d12;
                    size = vs.variants.d3d12Size;
                    break;
                default: // Vulkan / OpenGL / OpenGL ES / Metal consume SPIR-V (transpiled where needed)
                    code = vs.variants.spirv;
                    size = vs.variants.spirvSize;
                    break;
            }
            if (!code || size == 0)
                return MakeError(std::string("GraphicsPipelineBuilder::Build: no shader blob for backend ") +
                                 device.ApiName());

            VriShaderDesc shader {};
            shader.stage          = vs.stage;
            shader.bytecode       = code;
            shader.bytecodeSize   = size;
            shader.entryPointName = vs.entryPoint.c_str();
            shaders.push_back(shader);
        }

        if (shaders.empty())
            return MakeError("GraphicsPipelineBuilder::Build: no shader stages added");

        VriGraphicsPipelineDesc desc {};
        desc.pipelineLayout = m_layout;
        desc.shaders        = shaders.data();
        desc.shaderNum      = static_cast<uint32_t>(shaders.size());
        if (!m_vertexAttributes.empty())
        {
            desc.vertexInput.attributes   = m_vertexAttributes.data();
            desc.vertexInput.attributeNum = static_cast<uint32_t>(m_vertexAttributes.size());
        }
        if (!m_vertexStreams.empty())
        {
            desc.vertexInput.streams   = m_vertexStreams.data();
            desc.vertexInput.streamNum = static_cast<uint32_t>(m_vertexStreams.size());
        }
        desc.inputAssembly = m_inputAssembly;
        desc.rasterization = m_rasterization;
        desc.multisample   = m_multisample;
        desc.depthStencil  = m_depthStencil;

        desc.outputMerger.colors             = m_colorAttachments.empty() ? nullptr : m_colorAttachments.data();
        desc.outputMerger.colorNum           = static_cast<uint32_t>(m_colorAttachments.size());
        desc.outputMerger.depthStencilFormat = m_depthStencilFormat;
        desc.outputMerger.viewMask           = m_viewMask;

        VriPipeline*    pipeline = nullptr;
        const VriResult r        = device.Core().CreateGraphicsPipeline(device.Handle(), &desc, &pipeline);
        if (r != VriResult_Success)
            return MakeError(r, "GraphicsPipelineBuilder::Build", "CreateGraphicsPipeline failed");
        return pipeline;
    }
} // namespace vrf
