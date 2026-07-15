// Mesh-shader variant of the splat raster. SINGLE mesh entry point on purpose:
// Slang's multi-entry SPIR-V link drops the mesh OutputTrianglesEXT execution mode
// (a combined vertex+fragment+mesh module renders nothing), so the mesh stage lives
// alone here. The fragment stage reuses splat.slang's fragmentMain. Projection/SH
// logic is shared via splat_common.slangh.
import vsh;
#include "splat_common.slangh"

// Each workgroup expands kMeshGroup splats (in sorted order) into up to kMeshGroup
// quads (4 verts + 2 tris each). uDrawArgs[1] is the sort's visible (culled) count;
// groups past it emit nothing.
static const uint kMeshGroup = 32;

[shader("mesh")]
[numthreads(kMeshGroup, 1, 1)]
[outputtopology("triangle")]
void meshMain(uint gtid: SV_GroupThreadID,
              uint gid: SV_GroupID,
              out vertices VSOutput verts[kMeshGroup * 4],
              out indices uint3 tris[kMeshGroup * 2])
{
    const uint total = uDrawArgs[1]; // visible splats after frustum/size cull
    const uint base  = gid * kMeshGroup;
    const uint valid = (base < total) ? min(kMeshGroup, total - base) : 0u;

    SetMeshOutputCounts(valid * 4, valid * 2);
    if (gtid >= valid)
        return;

    const SplatQuad q = projectSplat(uSortedIndices[base + gtid]);
    VSOutput        clipped;
    clipped.clipPos = float4(0, 0, 2, 1); // degenerate for culled splats
    clipped.quadPos = float2(0, 0);
    clipped.color   = float4(0, 0, 0, 0);
    const uint v0 = gtid * 4;
    [[unroll]] for (uint c = 0; c < 4; ++c)
        verts[v0 + c] = q.visible ? emitCorner(q, c) : clipped;

    // Two triangles over the 4 corners (0,1,2)(2,1,3) - matches the strip winding.
    const uint p0 = gtid * 2;
    tris[p0 + 0]  = uint3(v0 + 0, v0 + 1, v0 + 2);
    tris[p0 + 1]  = uint3(v0 + 2, v0 + 1, v0 + 3);
}
