// GBuffer pass: rasterize a hardcoded cube (no vertex buffers) into two MRT
// targets (albedo, world-space normal) + depth. Exercises the vrf::fg
// framegraph's attachment declaration and dynamic-rendering path.
import vsh;

struct PushConstants
{
    float4x4 mvp;   // clip-space transform (column-major, matches glm)
    float4x4 model; // world transform
};
[[vk::push_constant]] PushConstants pc;

// 6 faces x 2 triangles x 3 vertices; unit cube centered at origin.
static const float3 kFaceNormals[6] = {
    float3(1, 0, 0), float3(-1, 0, 0),
    float3(0, 1, 0), float3(0, -1, 0),
    float3(0, 0, 1), float3(0, 0, -1),
};
static const float3 kFaceColors[6] = {
    float3(0.9, 0.2, 0.2), float3(0.2, 0.9, 0.2),
    float3(0.2, 0.2, 0.9), float3(0.9, 0.9, 0.2),
    float3(0.9, 0.2, 0.9), float3(0.2, 0.9, 0.9),
};
// Corner offsets within a face, in the face's (u, v) basis.
static const float2 kCorners[6] = {
    float2(-1, -1), float2(1, -1), float2(1, 1),
    float2(-1, -1), float2(1, 1), float2(-1, 1),
};

struct VSOutput
{
    float4 clipPos : SV_Position;
    float3 normal : NORMAL;
    float3 color : COLOR0;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId: SV_VertexID)
{
    const uint   face   = vertexId / 6;
    const float2 corner = kCorners[vertexId % 6];

    const float3 n = kFaceNormals[face];
    // Build a (u, v) basis perpendicular to the face normal.
    const float3 u = abs(n.y) > 0.5 ? float3(1, 0, 0) : normalize(cross(float3(0, 1, 0), n));
    const float3 v = cross(n, u);

    const float3 position = n + u * corner.x + v * corner.y; // cube of half-extent 1

    VSOutput o;
    o.clipPos = mul(pc.mvp, float4(position * 0.5, 1.0));
    o.normal  = mul((float3x3)pc.model, n);
    o.color   = kFaceColors[face];
    return o;
}

struct GBufferOut
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
};

[shader("fragment")]
GBufferOut fragmentMain(VSOutput input)
{
    GBufferOut o;
    o.albedo = float4(input.color, 1.0);
    o.normal = float4(normalize(input.normal) * 0.5 + 0.5, 1.0);
    return o;
}
