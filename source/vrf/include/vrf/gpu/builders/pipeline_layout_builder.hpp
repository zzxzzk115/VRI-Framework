/*
 * pipeline_layout_builder.hpp - fluent builder over VriPipelineLayoutDesc.
 */
#pragma once

#include <cstdint>
#include <vector>

#include <vri/vri.h>

#include "vrf/core/result.hpp"

namespace vrf
{
    class RenderDevice;

    class PipelineLayoutBuilder
    {
    public:
        PipelineLayoutBuilder& SetShaderStages(VriShaderStageFlags stages)
        {
            m_shaderStages = stages;
            return *this;
        }

        PipelineLayoutBuilder& AddPushConstant(uint32_t baseRegister, uint32_t size, VriShaderStageFlags stages)
        {
            m_pushConstants.push_back(VriPushConstantDesc {baseRegister, size, stages});
            return *this;
        }

        // Add a descriptor set (register space) with its ranges. Ranges are copied.
        PipelineLayoutBuilder& AddDescriptorSet(uint32_t registerSpace, std::vector<VriDescriptorRangeDesc> ranges)
        {
            m_sets.push_back(SetEntry {registerSpace, std::move(ranges)});
            return *this;
        }

        [[nodiscard]] Expected<VriPipelineLayout*> Build(RenderDevice& device) const;

    private:
        struct SetEntry
        {
            uint32_t                            registerSpace = 0;
            std::vector<VriDescriptorRangeDesc> ranges;
        };

        VriShaderStageFlags              m_shaderStages = VriShaderStage_AllGraphics;
        std::vector<VriPushConstantDesc> m_pushConstants;
        std::vector<SetEntry>            m_sets;
    };
} // namespace vrf
