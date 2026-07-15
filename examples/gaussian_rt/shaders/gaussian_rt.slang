// 3D Gaussian ray tracing (3DGRT), inline ray-query path. Per pixel: cast a
// primary ray and traverse the procedural AABB BLAS (one box per splat),
// evaluating each splat's gaussian response at the ray's closest approach to the
// splat center (Mahalanobis metric), accumulated front-to-back.
//
// Chunked k-buffer traversal: instead of committing one hit per full BVH
// traversal (O(hits * traversal) - a frame never finishes on million-splat
// scenes), each traversal *gathers* the kChunk nearest splats along the ray
// into a small sorted buffer without committing, blends that chunk front-to-
// back, then advances TMin past it and re-traverses. Traversal count drops from
// O(hits) to O(hits / kChunk). Reference: NVIDIA vk_gaussian_splatting (VK3DGRT)
// particle response, on VRI inline ray query.
import vsh;

static const float kShC0      = 0.28209479177387814;
static const uint  kChunk     = 16;    // nearest splats gathered per traversal
static const uint  kMaxPasses = 16;    // <= kChunk*kMaxPasses total blends
static const float kMinT      = 0.003; // transmittance cutoff
static const float kMinResp   = 0.02;  // gaussian response cull

struct Camera
{
    float4x4 invViewProj; // clip -> world (ray gen)
    float3   cameraPos;
    float    pad0;
    uint2    resolution;
    uint2    pad1;
};
[[vk::push_constant]] ConstantBuffer<Camera> uCam;

struct Splat
{
    float3 position;
    float  opacity;
    float3 scale;
    float  pad0;
    float4 rotation;
    float3 shDC;
    float  pad1;
};
[[vk::binding(0, 0)]] RaytracingAccelerationStructure uScene;
[[vk::binding(1, 0)]] StructuredBuffer<Splat>         uSplats;
[[vk::binding(2, 0)]] RWTexture2D<float4>             uOut;

float3x3 rotMat(float4 q)
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    return float3x3(1 - 2 * (y * y + z * z),
                    2 * (x * y - w * z),
                    2 * (x * z + w * y),
                    2 * (x * y + w * z),
                    1 - 2 * (x * x + z * z),
                    2 * (y * z - w * x),
                    2 * (x * z - w * y),
                    2 * (y * z + w * x),
                    1 - 2 * (x * x + y * y));
}

// Inverse covariance Sigma^-1 = R S^-2 R^T.
float3x3 invCovariance(Splat sp)
{
    const float3   inv2 = 1.0 / (exp(sp.scale) * exp(sp.scale));
    const float3x3 R    = rotMat(sp.rotation);
    const float3x3 RS   = float3x3(R[0] * inv2, R[1] * inv2, R[2] * inv2); // R * diag(inv2)
    return mul(RS, transpose(R));
}

// Ray o+t*d vs gaussian: returns the peak t (closest approach in the metric)
// and the response there. `resp` = 0 if the peak is behind tMin.
float gaussianAlongRay(uint idx, float3 o, float3 d, float tMin, out float resp)
{
    const Splat    sp  = uSplats[idx];
    const float3x3 A   = invCovariance(sp);
    const float3   om  = o - sp.position;
    const float3   Ad  = mul(A, d);
    const float    dAd = dot(d, Ad);
    resp               = 0.0;
    if (dAd < 1e-12)
        return -1.0;
    const float t = -dot(om, Ad) / dAd;
    if (t < tMin)
        return -1.0;
    const float3 x  = om + t * d;
    const float  m2 = dot(x, mul(A, x));
    resp            = exp(-0.5 * m2);
    return t;
}

[shader("compute")][numthreads(8, 8, 1)] void computeMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= uCam.resolution.x || tid.y >= uCam.resolution.y)
        return;

    const float2 uv     = (float2(tid.xy) + 0.5) / float2(uCam.resolution);
    const float2 ndc    = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float4 farW   = mul(uCam.invViewProj, float4(ndc, 1.0, 1.0));
    const float3 target = farW.xyz / farW.w;
    const float3 o      = uCam.cameraPos;
    const float3 d      = normalize(target - o);

    float3 color = float3(0.0);
    float  T     = 1.0;
    float  tMin  = 0.0;

    for (uint pass = 0; pass < kMaxPasses && T > kMinT; ++pass)
    {
        // --- gather the kChunk nearest splats (t >= tMin) in one traversal ---
        // gt/gi/gr kept sorted ascending by t; capacity kChunk, no commits so the
        // ray query visits every candidate box along the ray exactly once.
        float gt[kChunk];
        uint  gi[kChunk];
        float gr[kChunk];
        uint  n     = 0;
        float worst = 1e30; // largest kept t (meaningful once n == kChunk)

        RayDesc ray;
        ray.Origin    = o;
        ray.Direction = d;
        ray.TMin      = tMin;
        ray.TMax      = 1e5;

        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(uScene, RAY_FLAG_NONE, 0xFF, ray);
        while (q.Proceed())
        {
            if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE)
            {
                float       resp;
                const uint  idx = q.CandidatePrimitiveIndex();
                const float t   = gaussianAlongRay(idx, o, d, tMin, resp);
                if (t > 0.0 && resp > kMinResp && (n < kChunk || t < worst))
                {
                    // Insert into the sorted buffer; when full, evict the largest.
                    uint pos = (n < kChunk) ? n : (kChunk - 1);
                    if (n < kChunk)
                        ++n;
                    gt[pos] = t;
                    gi[pos] = idx;
                    gr[pos] = resp;
                    for (uint j = pos; j > 0 && gt[j] < gt[j - 1]; --j)
                    {
                        const float tt = gt[j];
                        gt[j]          = gt[j - 1];
                        gt[j - 1]      = tt;
                        const uint ii  = gi[j];
                        gi[j]          = gi[j - 1];
                        gi[j - 1]      = ii;
                        const float rr = gr[j];
                        gr[j]          = gr[j - 1];
                        gr[j - 1]      = rr;
                    }
                    worst = gt[n - 1];
                }
                // Never commit: traversal keeps visiting the remaining candidates.
            }
        }
        if (n == 0)
            break; // no more splats along the ray

        // --- blend the gathered chunk front-to-back ---
        for (uint k = 0; k < n && T > kMinT; ++k)
        {
            const Splat  sp    = uSplats[gi[k]];
            const float  alpha = saturate((1.0 / (1.0 + exp(-sp.opacity))) * gr[k]);
            const float3 rgb   = max(0.5 + kShC0 * sp.shDC, 0.0);
            color += T * alpha * rgb;
            T *= (1.0 - alpha);
        }

        if (n < kChunk)
            break;                 // buffer wasn't full => consumed every remaining splat
        tMin = gt[n - 1] + 1e-4;   // advance past the farthest splat in this chunk
    }

    uOut[tid.xy] = float4(color, 1.0 - T);
}
