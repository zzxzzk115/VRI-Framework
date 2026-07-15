// 3D Gaussian Splatting raster (EWA / 3DGUT): one instanced quad per splat,
// expanded to the projected 2D covariance ellipse, gaussian-weighted, premultiplied
// over-blended back-to-front (the host sorts). Shared projection/SH logic lives in
// splat_common.slangh; the mesh-shader variant is splat_mesh.slang.
import vsh;
#include "splat_common.slangh"

[shader("vertex")]
VSOutput vertexMain(uint vertexId: SV_VertexID, uint instanceId: SV_InstanceID)
{
    const SplatQuad q = projectSplat(uSortedIndices[instanceId]);
    if (!q.visible)
    {
        VSOutput o;
        o.clipPos = float4(0, 0, 2, 1); // clipped (behind far plane)
        o.quadPos = float2(0, 0);
        o.color   = float4(0, 0, 0, 0);
        return o;
    }
    return emitCorner(q, vertexId);
}

[shader("fragment")]
float4 fragmentMain(VSOutput i): SV_Target
{
    const float g = exp(-0.5 * dot(i.quadPos, i.quadPos));
    const float alpha = i.color.a * g;
    if (alpha < 1.0 / 255.0)
        discard;
    return float4(i.color.rgb * alpha, alpha); // premultiplied
}
