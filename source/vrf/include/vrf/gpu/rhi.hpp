/*
 * rhi.hpp - vrf::rhi handle aliases: the one adaptation point for VRI's C-ABI handle types.
 *
 * VRI hands ownership back as raw pointers to opaque handle types (VriBuffer, VriTexture, ...).
 * Those spellings currently appear directly across the framework's public headers, so a rename or
 * re-namespacing on the VRI side - the kind of churn the RHI is still going through - would ripple
 * across all of them. This header re-exports the handle set under stable vrf::rhi:: aliases so that,
 * going forward, code can name vrf::rhi::Buffer and only THIS file has to track VRI's spelling.
 *
 * The aliases are exact `using` typedefs - vrf::rhi::Buffer *is* ::VriBuffer - so the two names are
 * fully interchangeable: a raw VriBuffer* still binds to a vrf::rhi::Buffer* parameter and vice
 * versa. Adopting an alias is a behaviour-preserving no-op, which is what lets existing headers keep
 * their direct Vri* references untouched while new code prefers the aliases.
 *
 * Only the opaque handles are aliased here. Enums, descriptor/pipeline *Desc structs and VriResult
 * are intentionally left with their VRI spelling: they are consumed through their enumerators / by
 * value and vrf mirrors several of them 1:1 on purpose (see core/result.hpp).
 */
#pragma once

#include <vri/vri.h>

namespace vrf::rhi
{
    using Device           = ::VriDevice;
    using Queue            = ::VriQueue;
    using CommandAllocator = ::VriCommandAllocator;
    using CommandBuffer    = ::VriCommandBuffer;
    using Buffer           = ::VriBuffer;
    using Texture          = ::VriTexture;
    using Descriptor       = ::VriDescriptor; // a single resource view or sampler
    using DescriptorSet    = ::VriDescriptorSet;
    using DescriptorPool   = ::VriDescriptorPool;
    using PipelineLayout   = ::VriPipelineLayout;
    using Pipeline         = ::VriPipeline;
    using PipelineCache    = ::VriPipelineCache;
    using Memory           = ::VriMemory;
    using Fence            = ::VriFence;
    using SwapChain        = ::VriSwapChain;
} // namespace vrf::rhi
