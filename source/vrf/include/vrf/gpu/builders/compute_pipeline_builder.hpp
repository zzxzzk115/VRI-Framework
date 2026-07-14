/*
 * compute_pipeline_builder.hpp - fluent builder over VriComputePipelineDesc.
 *
 * The compute counterpart to GraphicsPipelineBuilder: one shader stage + a
 * pipeline layout. Like the graphics builder it accepts either a single raw
 * blob (SetShader) or per-backend variants (SetShaderVariants), resolving the
 * right blob for the active backend at Build().
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <vri/vri.h>

#include "vrf/core/result.hpp"
#include "vrf/gpu/builders/graphics_pipeline_builder.hpp" // ShaderVariants

namespace vrf
{
    class RenderDevice;

    class ComputePipelineBuilder
    {
    public:
        ComputePipelineBuilder() = default;

        ComputePipelineBuilder& SetPipelineLayout(VriPipelineLayout* layout)
        {
            m_layout = layout;
            return *this;
        }

        // Set the compute shader from a single raw blob. `bytecode` must outlive Build().
        ComputePipelineBuilder& SetShader(const void* bytecode, size_t size, const char* entryPoint = "main")
        {
            m_bytecode    = bytecode;
            m_size        = size;
            m_entryPoint  = entryPoint;
            m_hasVariants = false;
            return *this;
        }

        // Set the compute shader from per-backend variants; the blob for the active backend is
        // chosen at Build(). Blobs must outlive Build().
        ComputePipelineBuilder& SetShaderVariants(const ShaderVariants& variants, std::string entryPoint = "main")
        {
            m_variants    = variants;
            m_entryPoint  = std::move(entryPoint);
            m_hasVariants = true;
            return *this;
        }

        [[nodiscard]] Expected<VriPipeline*> Build(RenderDevice& device) const;

    private:
        VriPipelineLayout* m_layout   = nullptr;
        const void*        m_bytecode = nullptr;
        size_t             m_size     = 0;
        std::string        m_entryPoint {"main"};
        ShaderVariants     m_variants {};
        bool               m_hasVariants = false;
    };
} // namespace vrf
