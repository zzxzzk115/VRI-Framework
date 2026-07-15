// Procedural AABB ray query: a BLAS of one AABB containing a unit sphere. The
// ray query reports the box as a PROCEDURAL candidate; the shader does the
// ray-sphere test and commits the hit. Validates VRI's new AABB acceleration-
// structure geometry + inline procedural intersection on both backends.
import vsh;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure uScene;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> uOut; // grid*grid, hit distance (-1 = miss)

struct Params
{
    uint grid;
};
[[vk::push_constant]] ConstantBuffer<Params> uP;

// Ray-sphere (unit sphere at origin): nearest positive t, or -1.
float sphereHit(float3 o, float3 d, float tmin, float tmax)
{
    const float b    = dot(o, d);
    const float c    = dot(o, o) - 1.0;
    const float disc = b * b - c;
    if (disc < 0.0)
        return -1.0;
    const float t = -b - sqrt(disc);
    return (t >= tmin && t <= tmax) ? t : -1.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= uP.grid || tid.y >= uP.grid)
        return;
    const float2 uv  = (float2(tid.xy) + 0.5) / float(uP.grid);
    const float2 ndc = uv * 2.0 - 1.0;

    RayDesc ray;
    ray.Origin    = float3(ndc.x * 1.5, -ndc.y * 1.5, -3.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin      = 0.0;
    ray.TMax      = 10.0;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(uScene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE)
        {
            // Object-space ray (single identity instance here).
            const float t = sphereHit(ray.Origin, ray.Direction, q.RayTMin(), q.CommittedRayT());
            if (t > 0.0)
                q.CommitProceduralPrimitiveHit(t);
        }
    }

    const uint idx = tid.y * uP.grid + tid.x;
    uOut[idx] = (q.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT) ? q.CommittedRayT() : -1.0;
}
