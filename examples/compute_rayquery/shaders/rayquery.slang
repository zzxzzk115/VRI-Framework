// Inline ray tracing (RayQuery) validation - the compute counterpart to
// rt_triangle. Traces one orthographic ray per grid cell against a 1-triangle
// TLAS and writes the hit distance; no RT pipeline / SBT, so it runs on any
// backend that reports hasRayQuery (incl. Metal, which has no DXR-style SBT).
import vsh;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure uScene;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> uOut; // grid*grid, hit distance (-1 = miss)

struct Params
{
    uint grid;
};
[[vk::push_constant]] ConstantBuffer<Params> uP;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= uP.grid || tid.y >= uP.grid)
        return;
    const float2 uv  = (float2(tid.xy) + 0.5) / float(uP.grid);
    const float2 ndc = uv * 2.0 - 1.0;

    RayDesc ray;
    ray.Origin    = float3(ndc.x, -ndc.y, -2.0); // orthographic, looking down +z
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin      = 0.01;
    ray.TMax      = 10.0;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(uScene, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed(); // opaque triangles: one Proceed resolves the closest hit

    const uint idx = tid.y * uP.grid + tid.x;
    uOut[idx]      = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? q.CommittedRayT() : -1.0;
}
