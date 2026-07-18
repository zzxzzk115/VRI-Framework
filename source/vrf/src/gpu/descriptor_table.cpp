#include "vrf/gpu/descriptor_table.hpp"

#include <algorithm>

#include "vrf/gpu/builders/pipeline_layout_builder.hpp"
#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    VriDescriptorRangeDesc DescriptorTable::Range() const noexcept
    {
        VriDescriptorRangeDesc r {};
        r.baseRegister   = m_desc.baseRegister;
        r.descriptorNum  = m_desc.capacity;
        r.descriptorType = m_desc.type;
        r.shaderStages   = m_desc.stages;
        // True bindless: a variable-sized (runtime) array whose tail may be left unbound. Fallback:
        // a plain fixed array (every slot must be written - see Commit's fallback fill).
        r.flags    = m_bindless ? (VriDescriptorRange_VariableSized | VriDescriptorRange_PartiallyBound) :
                                  VriDescriptorRange_None;
        r.viewType = m_desc.viewType;
        return r;
    }

    Expected<DescriptorTable> DescriptorTable::Create(RenderDevice& device, const Desc& desc)
    {
        DescriptorTable t;
        t.m_device   = &device;
        t.m_desc     = desc;
        t.m_bindless = device.Desc()->hasBindless == VRI_TRUE;
        t.m_entries.assign(desc.capacity, desc.fallbackDescriptor);

        // 1) A minimal pipeline layout holding just this table's set, to allocate the set from.
        PipelineLayoutBuilder lb;
        lb.SetShaderStages(desc.stages);
        lb.AddDescriptorSet(desc.registerSpace, {t.Range()});
        auto layout = lb.Build(device);
        if (!layout)
        {
            return std::unexpected(layout.error());
        }
        t.m_layout = UniquePipelineLayout {device, *layout};

        // 2) A pool big enough for one set + `capacity` descriptors of the table's type.
        VriDescriptorPoolDesc pd {};
        pd.descriptorSetMaxNum = 1;
        switch (desc.type)
        {
            case VriDescriptorType_Texture:
                pd.textureMaxNum = desc.capacity;
                break;
            case VriDescriptorType_StorageTexture:
                pd.storageTextureMaxNum = desc.capacity;
                break;
            case VriDescriptorType_ConstantBuffer:
                pd.constantBufferMaxNum = desc.capacity;
                break;
            case VriDescriptorType_StructuredBuffer:
                pd.structuredBufferMaxNum = desc.capacity;
                break;
            case VriDescriptorType_StorageBuffer:
                pd.storageBufferMaxNum = desc.capacity;
                break;
            case VriDescriptorType_Sampler:
                pd.samplerMaxNum = desc.capacity;
                break;
            case VriDescriptorType_AccelerationStructure:
                pd.accelerationStructureMaxNum = desc.capacity;
                break;
            default:
                break;
        }
        VriDescriptorPool* pool = nullptr;
        if (const auto r = device.Core().CreateDescriptorPool(device.Handle(), &pd, &pool); !Succeeded(r))
        {
            return MakeError(r, "DescriptorTable::Create", "CreateDescriptorPool failed");
        }
        t.m_pool = UniqueDescriptorPool {device, pool};

        // 3) Allocate the one set (setIndex == the set's register space, per VRI's convention).
        if (const auto r =
                device.Core().AllocateDescriptorSets(pool, t.m_layout.get(), desc.registerSpace, &t.m_set, 1);
            !Succeeded(r))
        {
            return MakeError(r, "DescriptorTable::Create", "AllocateDescriptorSets failed");
        }
        return t;
    }

    void DescriptorTable::Write(uint32_t index, VriDescriptor* view)
    {
        if (index < m_desc.capacity)
        {
            m_entries[index] = view;
            m_count          = std::max(m_count, index + 1);
            m_dirty          = true;
        }
    }

    uint32_t DescriptorTable::Add(VriDescriptor* view)
    {
        if (m_count >= m_desc.capacity)
        {
            return m_desc.capacity;
        }
        const uint32_t index = m_count;
        Write(index, view);
        return index;
    }

    void DescriptorTable::Clear()
    {
        m_entries.assign(m_desc.capacity, m_desc.fallbackDescriptor);
        m_count = 0;
        m_dirty = true;
    }

    void DescriptorTable::Commit()
    {
        if (!m_dirty || !m_set)
        {
            return;
        }
        // Every bound slot must hold a valid descriptor on the fixed path; fill gaps with the fallback.
        if (m_desc.fallbackDescriptor)
        {
            for (auto& e : m_entries)
            {
                if (!e)
                {
                    e = m_desc.fallbackDescriptor;
                }
            }
        }
        const VriDescriptorRangeUpdateDesc update {
            .descriptors = m_entries.data(), .descriptorNum = m_desc.capacity, .baseDescriptor = 0};
        m_device->Core().UpdateDescriptorRanges(m_set, /*baseRange=*/0, /*rangeNum=*/1, &update);
        m_dirty = false;
    }
} // namespace vrf
