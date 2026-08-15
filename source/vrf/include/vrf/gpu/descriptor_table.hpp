/*
 * descriptor_table.hpp - an indexed descriptor table (its own descriptor set).
 *
 * Uses true hardware bindless - a runtime-sized, partially-bound descriptor array - when the
 * device supports it (VriDeviceDesc::hasBindless), and otherwise falls back to a fixed-capacity
 * descriptor array (the pattern non-bindless backends like Metal already use). Same API and same
 * shader either way: the shader indexes the array by an integer. On the fallback path every bound
 * slot must be a valid descriptor, so unwritten slots are filled with Desc::fallbackDescriptor on
 * Commit (mirroring a renderer that pads its material array with a 1x1 white texture).
 *
 * Wiring: declare a compatible set in your pipeline layout from Range()/RegisterSpace(), populate
 * with Write()/Add(), Commit(), then bind with CmdSetDescriptorSet(RegisterSpace(), DescriptorSet()).
 */
#pragma once

#include <cstdint>
#include <vector>

#include "vrf/gpu/vri_types.hpp"
#include <vri/vri.h>

#include "vrf/core/result.hpp"
#include "vrf/gpu/handle.hpp"

namespace vrf
{
    class RenderDevice;

    class DescriptorTable
    {
    public:
        struct Desc
        {
            DescriptorType      type {VriDescriptorType_Texture};
            uint32_t            capacity {1024};
            uint32_t            registerSpace {0}; // the set index in the shader
            uint32_t            baseRegister {0};  // binding within the set
            VriShaderStageFlags stages {VriShaderStage_AllGraphics | VriShaderStage_Compute};
            VriTextureViewType  viewType {VriTextureViewType_2D}; // Texture ranges only (WebGPU bakes it)
            DescriptorHandle*   fallbackDescriptor {nullptr};     // fills unwritten slots on Commit
        };

        DescriptorTable() = default;

        [[nodiscard]] static Expected<DescriptorTable> Create(RenderDevice&, const Desc&);

        // Stage a descriptor at an explicit index / append it. Effective after Commit().
        void                   Write(uint32_t index, DescriptorHandle* view);
        [[nodiscard]] uint32_t Add(DescriptorHandle* view); // returns index, or Capacity() on overflow
        void                   Clear();                     // reset staged entries (keeps the GPU set)
        void                   Commit();                    // flush staged writes to the GPU set

        [[nodiscard]] DescriptorSetHandle* DescriptorSet() const noexcept { return m_set; }
        [[nodiscard]] uint32_t             RegisterSpace() const noexcept { return m_desc.registerSpace; }
        [[nodiscard]] uint32_t             Capacity() const noexcept { return m_desc.capacity; }
        [[nodiscard]] uint32_t             Count() const noexcept { return m_count; }
        [[nodiscard]] bool                 IsBindless() const noexcept { return m_bindless; }

        // The set-layout range this table occupies, for building a compatible pipeline layout.
        [[nodiscard]] VriDescriptorRangeDesc Range() const noexcept;

    private:
        RenderDevice*                  m_device {nullptr};
        Desc                           m_desc {};
        bool                           m_bindless {false};
        UniquePipelineLayout           m_layout;        // minimal layout to allocate the set from
        UniqueDescriptorPool           m_pool;          // owns m_set
        DescriptorSetHandle*           m_set {nullptr}; // non-owning (freed with the pool)
        std::vector<DescriptorHandle*> m_entries;       // staged, size == capacity
        uint32_t                       m_count {0};
        bool                           m_dirty {false};
    };
} // namespace vrf
