#include "vrf/gpu/builders/pipeline_layout_builder.hpp"

#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    Expected<VriPipelineLayout*> PipelineLayoutBuilder::Build(RenderDevice& device) const
    {
        // The VriDescriptorSetDesc array points at each set's ranges vector; both stay
        // alive on the stack across the CreatePipelineLayout call below.
        std::vector<VriDescriptorSetDesc> sets;
        sets.reserve(m_sets.size());
        for (const SetEntry& entry : m_sets)
        {
            VriDescriptorSetDesc set {};
            set.registerSpace = entry.registerSpace;
            set.ranges        = entry.ranges.data();
            set.rangeNum      = static_cast<uint32_t>(entry.ranges.size());
            sets.push_back(set);
        }

        VriPipelineLayoutDesc desc {};
        desc.descriptorSets   = sets.empty() ? nullptr : sets.data();
        desc.descriptorSetNum = static_cast<uint32_t>(sets.size());
        desc.pushConstants    = m_pushConstants.empty() ? nullptr : m_pushConstants.data();
        desc.pushConstantNum  = static_cast<uint32_t>(m_pushConstants.size());
        desc.shaderStages     = m_shaderStages;

        VriPipelineLayout* layout = nullptr;
        const VriResult    r      = device.Core().CreatePipelineLayout(device.Handle(), &desc, &layout);
        if (r != VriResult_Success)
            return MakeError(r, "PipelineLayoutBuilder::Build", "CreatePipelineLayout failed");
        return layout;
    }
} // namespace vrf
