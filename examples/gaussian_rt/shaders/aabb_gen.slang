// AABB generation for 3DGRT: one world-space bounding box per gaussian splat,
// sized to a few standard deviations of its (rotated, scaled) extent. The boxes
// become the procedural BLAS the ray tracer traverses; the per-box hit test
// (in gaussian_rt.slang) does the real gaussian intersection.
import vsh;

static const float kBoundStdDev = 3.0; // clip each gaussian at 3 sigma

struct Params
{
    uint count;
};
[[vk::push_constant]] ConstantBuffer<Params> uP;

struct Splat // 64-byte GaussianSplatPoint
{
    float3 position;
    float  opacity;
    float3 scale;
    float  pad0;
    float4 rotation;
    float3 shDC;
    float  pad1;
};
[[vk::binding(0, 0)]] StructuredBuffer<Splat>   uSplats;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> uAabbs; // 6 floats per splat (min xyz, max xyz)

[shader("compute")][numthreads(64, 1, 1)] void computeMain(uint3 tid : SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= uP.count)
        return;

    const Splat  sp = uSplats[i];
    const float3 s  = exp(sp.scale) * kBoundStdDev;

    // Rotate the per-axis extents by |R| to get the world-space AABB half-size:
    // half = |R| * s, where |R| is the elementwise-abs rotation matrix.
    const float    x = sp.rotation.x, y = sp.rotation.y, z = sp.rotation.z, w = sp.rotation.w;
    const float3x3 R    = float3x3(1 - 2 * (y * y + z * z),
                                2 * (x * y - w * z),
                                2 * (x * z + w * y),
                                2 * (x * y + w * z),
                                1 - 2 * (x * x + z * z),
                                2 * (y * z - w * x),
                                2 * (x * z - w * y),
                                2 * (y * z + w * x),
                                1 - 2 * (x * x + y * y));
    const float3   half = mul(abs(R), s);

    const uint base  = i * 6;
    uAabbs[base + 0] = sp.position.x - half.x;
    uAabbs[base + 1] = sp.position.y - half.y;
    uAabbs[base + 2] = sp.position.z - half.z;
    uAabbs[base + 3] = sp.position.x + half.x;
    uAabbs[base + 4] = sp.position.y + half.y;
    uAabbs[base + 5] = sp.position.z + half.z;
}
