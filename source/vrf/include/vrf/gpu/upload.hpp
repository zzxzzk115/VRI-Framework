/*
 * upload.hpp - CPU asset -> GPU resource bridge.
 *
 * UploadMesh interleaves a Mesh's SoA attributes into a single vertex buffer plus
 * an index buffer; UploadTexture stages an uncompressed CPU Texture into a sampled
 * GPU texture. Both stage through a host-visible buffer and a one-shot submit.
 */
#pragma once

#include <cstdint>

#include <vri/vri.h>

#include "vrf/asset/mesh.hpp"
#include "vrf/asset/texture.hpp"
#include "vrf/core/result.hpp"
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
        VriBuffer*      vertexBuffer = nullptr;
        VriBuffer*      indexBuffer  = nullptr;
        uint32_t        vertexCount  = 0;
        uint32_t        indexCount   = 0;
        VriIndexType    indexType    = VriIndexType_UInt32;
        GpuVertexLayout layout;

        void Destroy(RenderDevice& device);
    };

    struct GpuTexture
    {
        VriTexture*    texture = nullptr;
        VriDescriptor* view    = nullptr;

        void Destroy(RenderDevice& device);
    };

    [[nodiscard]] Expected<GpuMesh>    UploadMesh(RenderDevice& device, const Mesh& mesh);
    [[nodiscard]] Expected<GpuTexture> UploadTexture(RenderDevice& device, const Texture& texture);
} // namespace vrf
