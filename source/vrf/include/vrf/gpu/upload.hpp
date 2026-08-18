/*
 * upload.hpp - CPU asset -> GPU resource bridge.
 *
 * UploadMesh interleaves a Mesh's SoA attributes into a single vertex buffer plus
 * an index buffer; UploadTexture stages an uncompressed CPU Texture into a sampled
 * GPU texture. Both stage through a host-visible buffer and a one-shot submit.
 */
#pragma once

#include <cstdint>
#include <functional>

#include <vri/vri.h>

#include "vrf/asset/mesh.hpp"
#include "vrf/asset/texture.hpp"
#include "vrf/core/result.hpp"
#include "vrf/gpu/handle.hpp"
#include "vrf/gpu/vertex_layout.hpp"

namespace vrf
{
    class RenderDevice;

    // A mesh uploaded into a single interleaved vertex buffer + an index buffer. `layout`
    // describes that buffer: which attributes it actually carries (in canonical order) and
    // their formats/offsets/stride. UploadMesh only writes the attributes the source mesh has
    // (generating tangents when normals+uvs are present but tangents are not), so the layout -
    // and thus the pipeline vertex attributes and the shader variant (VTX_HAS_*) - is driven by
    // the mesh, not a fixed vertex struct.
    struct GpuMesh
    {
        UniqueBuffer    vertexBuffer;
        UniqueBuffer    indexBuffer;
        uint32_t        vertexCount = 0;
        uint32_t        indexCount  = 0;
        VriIndexType    indexType   = VriIndexType_UInt32;
        GpuVertexLayout layout;
    };

    struct GpuTexture
    {
        UniqueTexture    texture;
        UniqueDescriptor view;
    };

    // rtAccelInput adds VriBufferUsage_AccelerationBuildInput to the vertex + index buffers so the
    // mesh can back a ray-tracing BLAS (Blas::Create). Requires a device with ray query / ray tracing.
    [[nodiscard]] Expected<GpuMesh>    UploadMesh(RenderDevice& device, const Mesh& mesh, bool rtAccelInput = false);
    // extraUsage is ORed into the created texture's usage - e.g.
    // VriTextureUsage_ConcurrentQueues for sampled assets a compute-queue pass also reads.
    [[nodiscard]] Expected<GpuTexture>
    UploadTexture(RenderDevice& device, const Texture& texture, VriTextureUsageFlags extraUsage = 0);

    // Record one-shot GPU work into a transient command buffer, submit, and wait. The public
    // form of the internal staging-submit used by UploadMesh/UploadTexture; handy for building
    // acceleration structures or other one-time device work outside a frame loop.
    void ImmediateSubmit(RenderDevice& device, const std::function<void(VriCommandBuffer*)>& record);
} // namespace vrf
