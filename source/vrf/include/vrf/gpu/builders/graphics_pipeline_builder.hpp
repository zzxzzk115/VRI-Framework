/*
 * graphics_pipeline_builder.hpp - fluent builder over VriGraphicsPipelineDesc.
 *
 * VRI itself takes a plain aggregate VriGraphicsPipelineDesc; this builder adds
 * the fluent .SetX().AddShader().Build(device) surface the framework standardizes
 * on, with sensible defaults (fill / no-cull / CCW / 1x MSAA, no depth).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <vri/vri.h>

#include "vrf/core/result.hpp"
#include "vrf/gpu/vertex_layout.hpp"

namespace vrf
{
    class RenderDevice;

    // Per-backend shader blobs. VRI takes raw bytecode; which blob to feed is a backend
    // choice - SPIR-V for Vulkan / OpenGL / Metal (the GL/Metal backends transpile it),
    // WGSL for WebGPU, DXBC/DXIL for D3D12. AddShaderVariants picks the right one at Build()
    // from the device's resolved backend, so a pipeline is not locked to one API. (SPIR-V and
    // WGSL modules can hold multiple entry points; D3D12 blobs are one stage each.)
    struct ShaderVariants
    {
        const void* spirv     = nullptr;
        size_t      spirvSize = 0;
        const void* wgsl      = nullptr;
        size_t      wgslSize  = 0;
        const void* d3d12     = nullptr; // DXBC (sm5.1) or DXIL (sm6.x)
        size_t      d3d12Size = 0;
    };

    class GraphicsPipelineBuilder
    {
    public:
        GraphicsPipelineBuilder();

        GraphicsPipelineBuilder& SetPipelineLayout(VriPipelineLayout* layout)
        {
            m_layout = layout;
            return *this;
        }

        // Add a shader stage from a single raw blob. `bytecode` must outlive Build().
        GraphicsPipelineBuilder&
        AddShader(VriShaderStageBits stage, const void* bytecode, size_t size, const char* entryPoint = "main");

        // Add a shader stage from per-backend variants; the blob for the active backend is
        // chosen at Build(). Blobs must outlive Build().
        GraphicsPipelineBuilder&
        AddShaderVariants(VriShaderStageBits stage, const ShaderVariants& variants, std::string entryPoint = "main");

        GraphicsPipelineBuilder& SetTopology(VriPrimitiveTopology topology)
        {
            m_inputAssembly.topology = topology;
            return *this;
        }

        // Treat the max index value (0xFFFF / 0xFFFFFFFF for the bound index type) as a strip
        // break instead of a vertex. Required for indexed *Strip / *Fan topologies that pack
        // many runs into one draw - e.g. a per-row grid mesh as a single triangle strip.
        GraphicsPipelineBuilder& SetPrimitiveRestart(bool enabled)
        {
            m_inputAssembly.primitiveRestart = enabled ? VRI_TRUE : VRI_FALSE;
            return *this;
        }

        // Vertices per patch for a tessellated pipeline (0 = no tessellation). Pair with
        // SetTopology(VriPrimitiveTopology_PatchList) and tess control/eval shader stages;
        // gate on VriDeviceDesc::hasTessellation.
        GraphicsPipelineBuilder& SetPatchControlPoints(uint32_t patchControlPoints)
        {
            m_tessellation.patchControlPoints = patchControlPoints;
            return *this;
        }

        GraphicsPipelineBuilder& SetCullMode(VriCullMode cullMode)
        {
            m_rasterization.cullMode = cullMode;
            return *this;
        }

        GraphicsPipelineBuilder& SetPolygonMode(VriPolygonMode polygonMode)
        {
            m_rasterization.polygonMode = polygonMode;
            return *this;
        }

        GraphicsPipelineBuilder& SetFrontFace(VriFrontFace frontFace)
        {
            m_rasterization.frontFace = frontFace;
            return *this;
        }

        GraphicsPipelineBuilder& SetLineWidth(float lineWidth)
        {
            m_rasterization.lineWidth = lineWidth;
            return *this;
        }

        GraphicsPipelineBuilder& SetSampleCount(uint32_t sampleNum)
        {
            m_multisample.sampleNum = sampleNum;
            return *this;
        }

        // Vertex input. Add a stream (vertex buffer binding) then its attributes; attributes
        // are matched to shader input locations by the order they are added.
        GraphicsPipelineBuilder& AddVertexStream(uint32_t          stride,
                                                 uint32_t          bindingSlot = 0,
                                                 VriVertexStepRate stepRate    = VriVertexStepRate_PerVertex)
        {
            m_vertexStreams.push_back(VriVertexStreamDesc {stride, bindingSlot, stepRate});
            return *this;
        }

        GraphicsPipelineBuilder& AddVertexAttribute(VriFormat format, uint32_t offset, uint32_t streamIndex = 0)
        {
            VriVertexAttributeDesc attribute {};
            attribute.format      = format;
            attribute.offset      = offset;
            attribute.streamIndex = streamIndex;
            m_vertexAttributes.push_back(attribute);
            return *this;
        }

        // Add a single interleaved stream from a compact GpuVertexLayout: the stride plus each
        // present attribute in canonical order, so attribute index N maps to shader input location
        // N (matching a shader variant compiled for the same vertex-attribute mask).
        GraphicsPipelineBuilder& AddVertexLayout(const GpuVertexLayout& layout, uint32_t bindingSlot = 0)
        {
            AddVertexStream(layout.stride, bindingSlot);
            for (const GpuVertexAttribute& a : layout.attributes)
                AddVertexAttribute(a.format, a.offset, bindingSlot);
            return *this;
        }

        // Add a color attachment (matched by index to the render's attachments).
        GraphicsPipelineBuilder& AddColorAttachment(VriFormat          format,
                                                    VriColorWriteFlags writeMask = VriColorWrite_RGBA,
                                                    VriBlendDesc       blend     = {});

        GraphicsPipelineBuilder& SetDepthStencilFormat(VriFormat format)
        {
            m_depthStencilFormat = format;
            return *this;
        }

        GraphicsPipelineBuilder& SetDepthTest(bool test, bool write, VriCompareOp compareOp = VriCompareOp_LessOrEqual)
        {
            m_depthStencil.depthTest      = test ? VRI_TRUE : VRI_FALSE;
            m_depthStencil.depthWrite     = write ? VRI_TRUE : VRI_FALSE;
            m_depthStencil.depthCompareOp = compareOp;
            return *this;
        }

        // Multiview render targets (0 = single view). 0x3 renders both layers of
        // a 2-layer array target in one pass (stereo). Must match the render's
        // VriAttachmentsDesc::viewMask; gate on VriDeviceDesc::hasMultiview.
        GraphicsPipelineBuilder& SetViewMask(uint32_t viewMask)
        {
            m_viewMask = viewMask;
            return *this;
        }

        [[nodiscard]] Expected<VriPipeline*> Build(RenderDevice& device) const;

    private:
        struct VariantShader
        {
            VriShaderStageBits stage;
            std::string        entryPoint;
            ShaderVariants     variants;
        };

        VriPipelineLayout*                  m_layout = nullptr;
        std::vector<VriShaderDesc>          m_shaders;
        std::vector<VariantShader>          m_variantShaders;
        std::vector<VriColorAttachmentDesc> m_colorAttachments;
        std::vector<VriVertexAttributeDesc> m_vertexAttributes;
        std::vector<VriVertexStreamDesc>    m_vertexStreams;
        VriInputAssemblyDesc                m_inputAssembly {};
        VriTessellationDesc                 m_tessellation {};
        VriRasterizationDesc                m_rasterization {};
        VriMultisampleDesc                  m_multisample {};
        VriDepthStencilDesc                 m_depthStencil {};
        VriFormat                           m_depthStencilFormat = VriFormat_Unknown;
        uint32_t                            m_viewMask           = 0;
    };
} // namespace vrf
