// rt_inpaint - the ray-traced-inpaint SHADING CORE, inline ray-query path.
//
// Per pixel: cast a primary ray into a TRIANGLE BLAS (a synthetic scene), take the
// closest committed triangle hit, fetch its 3 vertices (non-indexed geometry, so
// vertex k of triangle `prim` is 3*prim+k), derive the GEOMETRIC normal (cross
// product of two edges), interpolate the per-vertex albedo by the hit barycentrics,
// and apply directional lambertian shading. A miss returns a sky gradient.
//
// This is exactly the work the pixelwise-viewpoint-warping RT inpaint does at each
// disocclusion hole: trace a ray for the missing pixel and re-shade the scene it hits.
// Proven here on the native Metal backend (hasRayQuery) before wiring into the warp
// pipeline. Triangle path (committed hit), unlike gaussian_rt's procedural-AABB gather.
import vsh;

struct Camera
{
    float4x4 invViewProj; // clip -> world (ray gen)
    float3   cameraPos;
    float    pad0;
    uint2    resolution;
    uint2    pad1;
    float3   lightDir; // world-space, points FROM light (normalized)
    float    pad2;
};
[[vk::push_constant]] ConstantBuffer<Camera> uCam;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure uScene;
[[vk::binding(1, 0)]] StructuredBuffer<float4>        uVertices; // .xyz position (stride 16), non-indexed
[[vk::binding(2, 0)]] StructuredBuffer<float4>        uColors;   // .rgb per-vertex albedo
[[vk::binding(3, 0)]] RWTexture2D<float4>             uOut;

[shader("compute")][numthreads(8, 8, 1)] void computeMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= uCam.resolution.x || tid.y >= uCam.resolution.y)
        return;

    // Primary ray from the inverse view-projection (no framebuffer Y flip - this is
    // a compute ray tracer writing a storage image directly, like gaussian_rt).
    const float2 uv     = (float2(tid.xy) + 0.5) / float2(uCam.resolution);
    const float2 ndc    = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float4 farW   = mul(uCam.invViewProj, float4(ndc, 1.0, 1.0));
    const float3 target = farW.xyz / farW.w;
    const float3 o      = uCam.cameraPos;
    const float3 d      = normalize(target - o);

    RayDesc ray;
    ray.Origin    = o;
    ray.Direction = d;
    ray.TMin      = 1e-3;
    ray.TMax      = 1e5;

    // Opaque triangle geometry: traversal commits the closest hit automatically.
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(uScene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed())
    {
        // No non-opaque / procedural candidates in this scene: nothing to do.
    }

    float3 color;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        const uint   prim = q.CommittedPrimitiveIndex();
        const uint   i0   = prim * 3 + 0; // non-indexed geometry
        const uint   i1   = prim * 3 + 1;
        const uint   i2   = prim * 3 + 2;
        const float3 v0   = uVertices[i0].xyz;
        const float3 v1   = uVertices[i1].xyz;
        const float3 v2   = uVertices[i2].xyz;

        // Geometric normal, oriented to face the ray (two-sided shading).
        float3 N = normalize(cross(v1 - v0, v2 - v0));
        if (dot(N, d) > 0.0)
            N = -N;

        // Albedo interpolated across the triangle by the hit barycentrics.
        const float2 bc   = q.CommittedTriangleBarycentrics();
        const float  b0   = 1.0 - bc.x - bc.y;
        const float3 albedo = b0 * uColors[i0].rgb + bc.x * uColors[i1].rgb + bc.y * uColors[i2].rgb;

        const float3 L       = -uCam.lightDir;
        const float  diffuse = max(dot(N, L), 0.0);
        const float  ambient = 0.20;
        color                = albedo * (ambient + 0.85 * diffuse);
    }
    else
    {
        // Sky gradient (miss).
        const float t = saturate(0.5 * (d.y + 1.0));
        color         = lerp(float3(0.55, 0.60, 0.70), float3(0.10, 0.16, 0.30), t);
    }

    uOut[tid.xy] = float4(color, 1.0);
}
