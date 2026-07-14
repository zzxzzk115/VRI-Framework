// 3D Gaussian Splatting raster: one instanced quad per splat, expanded to the
// projected 2D covariance ellipse, gaussian-weighted, premultiplied over-blended
// back-to-front (the host sorts). SH is DC-only here (band 0 = base color);
// higher bands are a later upgrade. Covariance math mirrors libvultra's
// gaussian_splat.glsl (EWA splatting, Zwicker et al.).
import vsh;

static const float kSplatExtentStdDev = 2.8284271247461903; // ~2*sqrt(2)
static const float kShC0              = 0.28209479177387814; // SH band-0 basis

struct Camera
{
    float4x4 view;       // world -> view
    float4x4 proj;       // view -> clip (0..1 depth)
    float2   focal;      // focal length in pixels (x, y)
    float2   viewport;   // render target size in pixels
    float    splatScale; // global size multiplier
    float    pad0;
    float2   pad1;
};
[[vk::binding(0, 0)]] ConstantBuffer<Camera> uCamera;

// Matches GaussianSplatPoint (64 bytes): position, opacity(logit), scale(log),
// rotation(quat xyzw), shDC.
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
[[vk::binding(1, 0)]] StructuredBuffer<Splat> uSplats;
[[vk::binding(2, 0)]] StructuredBuffer<uint> uSortedIndices; // back-to-front

struct VSOutput
{
    float4 clipPos : SV_Position;
    float2 quadPos : TEXCOORD0; // in std-dev units; gaussian falloff arg
    float4 color : TEXCOORD1;   // rgb premultipliable, a = peak opacity
};

// 3x3 covariance from log-scale + quaternion: R S S^T R^T.
float3x3 covariance3D(float3 logScale, float4 q)
{
    const float3 s = exp(logScale);
    // quaternion (xyzw) -> rotation matrix
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float3x3 R = float3x3(1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                                2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                                2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y));
    const float3x3 M = float3x3(s.x, 0, 0, 0, s.y, 0, 0, 0, s.z);
    const float3x3 RS = mul(R, M);
    return mul(RS, transpose(RS));
}

// Project cov3D to the 2D screen covariance (a, b, c) = (xx, xy, yy) in pixels^2.
float3 covariance2D(float3x3 cov3D, float3 centerView, float2 focal, float4x4 view)
{
    const float z = centerView.z;
    const float s = 1.0 / max(z * z, 1e-12);
    const float3x3 J = float3x3(focal.x / z, 0, -(focal.x * centerView.x) * s,
                                0, focal.y / z, -(focal.y * centerView.y) * s,
                                0, 0, 0);
    const float3x3 W = float3x3(view[0].xyz, view[1].xyz, view[2].xyz); // upper-left of view
    const float3x3 T = mul(J, W);
    const float3x3 cov2 = mul(mul(T, cov3D), transpose(T));
    return float3(cov2[0][0], cov2[0][1], cov2[1][1]);
}

[shader("vertex")]
VSOutput vertexMain(uint vertexId: SV_VertexID, uint instanceId: SV_InstanceID)
{
    VSOutput o;
    o.clipPos = float4(0, 0, 2, 1); // default: clipped (behind far plane)
    o.quadPos = float2(0, 0);
    o.color   = float4(0, 0, 0, 0);

    const Splat sp = uSplats[uSortedIndices[instanceId]];

    const float4 centerView = mul(uCamera.view, float4(sp.position, 1.0));
    if (centerView.z >= -0.01) // behind / at camera
        return o;

    const float3x3 cov3D = covariance3D(sp.scale, sp.rotation);
    float3 cov2 = covariance2D(cov3D, centerView.xyz, uCamera.focal, uCamera.view);

    // low-pass blur (dilate by 0.3px) + opacity compensation
    const float detOrig = cov2.x * cov2.z - cov2.y * cov2.y;
    cov2.x += 0.3;
    cov2.z += 0.3;
    const float detBlur = cov2.x * cov2.z - cov2.y * cov2.y;

    const float a = cov2.x, d = cov2.z, b = cov2.y;
    const float trace = a + d;
    const float term  = sqrt(max(0.01, 0.25 * trace * trace - (a * d - b * b)));
    const float lambda1 = 0.5 * trace + term;
    const float lambda2 = 0.5 * trace - term;
    if (lambda2 <= 0.0)
        return o;

    const float2 ev1 = normalize(float2((abs(b) < 1e-4) ? 1.0 : b, lambda1 - a));
    const float2 ev2 = float2(ev1.y, -ev1.x);
    const float2 basis1 = ev1 * uCamera.splatScale * min(kSplatExtentStdDev * sqrt(lambda1), 512.0);
    const float2 basis2 = ev2 * uCamera.splatScale * min(kSplatExtentStdDev * sqrt(lambda2), 512.0);

    // quad corners as a triangle strip (4 verts): (-1,-1)(1,-1)(-1,1)(1,1)
    const float2 corner = float2((vertexId & 1) ? 1.0 : -1.0, (vertexId & 2) ? 1.0 : -1.0);

    const float4 centerClip = mul(uCamera.proj, centerView);
    const float2 offsetPix  = corner.x * basis1 + corner.y * basis2;
    const float2 offsetNdc  = offsetPix / (0.5 * uCamera.viewport) * centerClip.w;

    o.clipPos = centerClip + float4(offsetNdc, 0, 0);
    o.quadPos = corner * kSplatExtentStdDev;

    float opacity = 1.0 / (1.0 + exp(-sp.opacity)); // sigmoid
    opacity *= sqrt(max(detOrig / max(detBlur, 1e-12), 0.0));
    const float3 rgb = max(0.5 + kShC0 * sp.shDC, 0.0);
    o.color = float4(rgb, opacity);
    return o;
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
