/*
 * vri_types.hpp - the single adaptation seam between vrf's public API and VRI's C-ABI type names.
 *
 * VRI's opaque handle types (VriBuffer, VriTexture, VriPipeline, ...) leak into ~30 of vrf's public
 * headers: every builder output, resource cache, the frame stream, the swapchain. A rename or
 * re-namespacing on the VRI side - the kind of churn the RHI is still going through - would otherwise
 * ripple across every one of those headers at once. This header re-exports the VRI types vrf's public
 * API touches under stable vrf:: aliases, so the rest of the framework names vrf::BufferHandle instead
 * of ::VriBuffer and only THIS file has to track VRI's spelling. That is the "few adaptation points"
 * the isolation is meant to converge on: when VRI moves, you edit here, not across the tree.
 *
 * The aliases are exact `using` typedefs - vrf::BufferHandle *is* ::VriBuffer - so an alias and the
 * raw VRI name stay fully interchangeable: adopting them is a behaviour-preserving no-op, a raw
 * VriBuffer* still binds to a vrf::BufferHandle* parameter, and code that has not been migrated keeps
 * compiling. Values of the aliased enums keep their Vri* spelling (VriFormat_RGBA8_UNORM, ...); only
 * the type name is aliased, which is the part a rename would break.
 *
 * Deliberately NOT aliased, because coupling there is intentional or localized rather than a blast
 * radius to shrink:
 *   - VriResult / the descriptor + pipeline *Desc structs: vrf mirrors these 1:1 on purpose (see
 *     core/result.hpp) - hiding the spelling would obscure that intent, not protect against it.
 *   - one-off enums a single builder wraps (VriPolygonMode, VriCompareOp, ...): confined to one
 *     header already, so they are not part of the wide surface this seam exists to isolate.
 */
#pragma once

#include <vri/vri.h>

namespace vrf
{
    // ---- Opaque resource / device handles -------------------------------------------------------
    // The primary blast-radius surface: passed by pointer through nearly every gpu/ and fg/ header,
    // owned by Unique<T> (gpu/handle.hpp), and reinterpret_cast to the backend impl behind the ABI.
    using DeviceHandle           = ::VriDevice;
    using QueueHandle            = ::VriQueue;
    using CommandAllocatorHandle = ::VriCommandAllocator;
    using CommandBufferHandle    = ::VriCommandBuffer;
    using BufferHandle           = ::VriBuffer;
    using TextureHandle          = ::VriTexture;
    using DescriptorHandle       = ::VriDescriptor; // a single resource view or sampler
    using DescriptorSetHandle    = ::VriDescriptorSet;
    using DescriptorPoolHandle   = ::VriDescriptorPool;
    using PipelineLayoutHandle   = ::VriPipelineLayout;
    using PipelineHandle         = ::VriPipeline;
    using FenceHandle            = ::VriFence;
    using SwapChainHandle        = ::VriSwapChain;

    // ---- Scoped-enum types the public API exposes ----------------------------------------------
    // Only the enum *types* are aliased (enum values stay Vri*_ spelled - see the file header), and
    // only where the short name does not shadow an existing accessor. Notably ::VriFormat and
    // ::VriPresentMode are deliberately left un-aliased: Swapchain already exposes Format() and
    // PresentMode() accessors, so a `Format`/`PresentMode` type alias would collide with the method
    // name and hide the type. Those two keep their Vri* spelling; the rename risk there is small
    // since they are almost always named through their VriFormat_*/VriPresentMode_* enumerators.
    using QueueType      = ::VriQueueType;
    using DescriptorType = ::VriDescriptorType;
    using MemoryLocation = ::VriMemoryLocation;
    using TextureType    = ::VriTextureType;
} // namespace vrf
