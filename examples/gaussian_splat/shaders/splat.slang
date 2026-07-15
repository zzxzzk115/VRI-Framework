// 3D Gaussian Splatting raster (EWA / 3DGUT). Two geometry paths from one module:
//  - vertexMain: one instanced quad per splat (the default path).
//  - meshMain:   a mesh shader expanding kMeshGroup sorted splats into quads.
// Both project each splat to its screen-space 2D covariance ellipse, gaussian-
// weighted and premultiplied over-blended back-to-front (the host sorts). The mesh
// and vertex entries share projectSplat/emitCorner and coexist in this one module -
// vshaderc emits a separate SPIR-V per entry, so the mesh OutputTrianglesEXT mode is
// preserved (no need to split the mesh stage into its own file).
import vsh;

static const float kSplatExtentStdDev = 2.8284271247461903; // ~2*sqrt(2)
static const float kShC0              = 0.28209479177387814; // SH band-0 basis

// Higher-order SH basis constants (bands 1-3), standard 3DGS convention.
static const float kShC1    = 0.4886025119029199;
static const float kShC2[5] = {1.0925484, -1.0925484, 0.3153916, -1.0925484, 0.5462742};
static const float kShC3[7] = {-0.5900435899266435, 2.890611442640554,  -0.4570457994644658, 0.3731763325901154,
                               -0.4570457994644658, 1.445305721320277,  -0.5900435899266435};

struct Camera
{
    float4x4 view;        // world -> view
    float4x4 proj;        // view -> clip (0..1 depth)
    float3   cameraPos;   // world-space eye (SH view direction)
    float    splatScale;  // global size multiplier
    float2   focal;       // focal length in pixels (x, y)
    float2   viewport;    // render target size in pixels
    uint     shDegree;    // 0..3 (0 = DC only)
    uint     shStride;    // higher-order floats per point = coeffCount(shDegree) * 3
    uint     antialiased; // 1 = Mip-Splatting opacity compensation, 0 = classic 3DGS
    uint     projection;  // 0 = EWA affine (Jacobian), 1 = 3DGUT unscented transform
};
[[vk::binding(0, 0)]] ConstantBuffer<Camera> uCamera;

// Matches GaussianSplatPoint (64 bytes).
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
[[vk::binding(2, 0)]] StructuredBuffer<uint>  uSortedIndices; // back-to-front
// Higher-order SH, flattened: [point][coeff][channel], channel innermost (spz).
[[vk::binding(3, 0)]] StructuredBuffer<float> uSH;
// Indirect draw args from the sort/cull: [0]=vertsPerInstance, [1]=visibleCount.
// Only the mesh-shader path reads it (to bound its per-group splat count).
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> uDrawArgs;

struct VSOutput
{
    float4 clipPos : SV_Position;
    float2 quadPos : TEXCOORD0; // in std-dev units; gaussian falloff arg
    float4 color : TEXCOORD1;   // rgb premultipliable, a = peak opacity
};

// One rest-SH coefficient (rgb) for point `p`, coefficient `c` (0-based).
float3 shCoeff(uint p, uint c)
{
    const uint base = p * uCamera.shStride + c * 3;
    return float3(uSH[base + 0], uSH[base + 1], uSH[base + 2]);
}

// View-dependent color from the higher-order bands (band 0 is the DC term).
float3 evalShRest(uint p, float3 dir)
{
    if (uCamera.shDegree == 0)
        return float3(0.0);

    const float x = dir.x, y = dir.y, z = dir.z;
    float3 rgb = kShC1 * (-shCoeff(p, 0) * y + shCoeff(p, 1) * z - shCoeff(p, 2) * x);
    if (uCamera.shDegree < 2)
        return rgb;

    const float xx = x * x, yy = y * y, zz = z * z, xy = x * y, yz = y * z, xz = x * z;
    rgb += kShC2[0] * xy * shCoeff(p, 3) + kShC2[1] * yz * shCoeff(p, 4) +
           kShC2[2] * (2.0 * zz - xx - yy) * shCoeff(p, 5) + kShC2[3] * xz * shCoeff(p, 6) +
           kShC2[4] * (xx - yy) * shCoeff(p, 7);
    if (uCamera.shDegree < 3)
        return rgb;

    rgb += kShC3[0] * (3.0 * xx - yy) * y * shCoeff(p, 8) + kShC3[1] * xy * z * shCoeff(p, 9) +
           kShC3[2] * (4.0 * zz - xx - yy) * y * shCoeff(p, 10) +
           kShC3[3] * z * (2.0 * zz - 3.0 * xx - 3.0 * yy) * shCoeff(p, 11) +
           kShC3[4] * x * (4.0 * zz - xx - yy) * shCoeff(p, 12) + kShC3[5] * (xx - yy) * z * shCoeff(p, 13) +
           kShC3[6] * x * (xx - 3.0 * yy) * shCoeff(p, 14);
    return rgb;
}

// 3x3 covariance from log-scale + quaternion: R S S^T R^T.
float3x3 covariance3D(float3 logScale, float4 q)
{
    const float3 s = exp(logScale);
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
    const float3x3 W = float3x3(view[0].xyz, view[1].xyz, view[2].xyz);
    const float3x3 T = mul(J, W);
    const float3x3 cov2 = mul(mul(T, cov3D), transpose(T));
    return float3(cov2[0][0], cov2[0][1], cov2[1][1]);
}

// 3DGUT: unscented-transform projection (6 sigma points through the full
// perspective projection, refit to 2D). Recovers EWA for near-linear projection;
// stays accurate under nonlinearity (wide FOV / off-axis) where the Jacobian drifts.
float3 covariance2DUnscented(float3 logScale, float4 q, float3 centerView, float2 focal, float4x4 view)
{
    const float3 s = exp(logScale);
    const float  x = q.x, y = q.y, z = q.z, w = q.w;
    const float3x3 R = float3x3(1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                                2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                                2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y));
    const float3x3 RS = mul(R, float3x3(s.x, 0, 0, 0, s.y, 0, 0, 0, s.z)); // columns = scaled axes
    const float3x3 W  = float3x3(view[0].xyz, view[1].xyz, view[2].xyz);

    const float3 a0 = mul(W, float3(RS[0][0], RS[1][0], RS[2][0]));
    const float3 a1 = mul(W, float3(RS[0][1], RS[1][1], RS[2][1]));
    const float3 a2 = mul(W, float3(RS[0][2], RS[1][2], RS[2][2]));

    const float sq = sqrt(3.0); // sigma-point scale for n=3, kappa=0 (weights 1/6 each)

    float2 pts[6];
    [[unroll]] for (uint k = 0; k < 3; ++k)
    {
        const float3 ax = (k == 0) ? a0 : ((k == 1) ? a1 : a2);
        const float3 pp = centerView + sq * ax;
        const float3 pm = centerView - sq * ax;
        pts[2 * k + 0] = focal * pp.xy / pp.z;
        pts[2 * k + 1] = focal * pm.xy / pm.z;
    }

    float2 mean = float2(0, 0);
    [[unroll]] for (uint i = 0; i < 6; ++i)
        mean += pts[i];
    mean *= (1.0 / 6.0);

    float3 cov = float3(0, 0, 0);
    [[unroll]] for (uint j = 0; j < 6; ++j)
    {
        const float2 d = pts[j] - mean;
        cov += float3(d.x * d.x, d.x * d.y, d.y * d.y);
    }
    return cov * (1.0 / 6.0);
}

// A splat projected to its screen-space quad.
struct SplatQuad
{
    bool   visible;
    float4 centerClip;
    float2 basis1;
    float2 basis2;
    float3 rgb;
    float  opacity;
};

SplatQuad projectSplat(uint splatIndex)
{
    SplatQuad q;
    q.visible = false;

    const Splat  sp         = uSplats[splatIndex];
    const float4 centerView = mul(uCamera.view, float4(sp.position, 1.0));
    if (centerView.z >= -0.01) // behind / at camera
        return q;

    float3 cov2;
    if (uCamera.projection != 0)
        cov2 = covariance2DUnscented(sp.scale, sp.rotation, centerView.xyz, uCamera.focal, uCamera.view);
    else
        cov2 = covariance2D(covariance3D(sp.scale, sp.rotation), centerView.xyz, uCamera.focal, uCamera.view);

    // Low-pass blur: dilate by 0.3px (both modes). Mip-Splatting opacity
    // compensation for the added energy is applied only when antialiased.
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
        return q;

    const float2 ev1 = normalize(float2((abs(b) < 1e-4) ? 1.0 : b, lambda1 - a));
    const float2 ev2 = float2(ev1.y, -ev1.x);
    q.basis1 = ev1 * uCamera.splatScale * min(kSplatExtentStdDev * sqrt(lambda1), 512.0);
    q.basis2 = ev2 * uCamera.splatScale * min(kSplatExtentStdDev * sqrt(lambda2), 512.0);
    q.centerClip = mul(uCamera.proj, centerView);

    float opacity = 1.0 / (1.0 + exp(-sp.opacity)); // sigmoid
    if (uCamera.antialiased != 0)
        opacity *= sqrt(max(detOrig / max(detBlur, 1e-12), 0.0)); // Mip-Splatting compensation
    const float3 dir = normalize(uCamera.cameraPos - sp.position);
    q.rgb            = max(0.5 + kShC0 * sp.shDC + evalShRest(splatIndex, dir), 0.0);
    q.opacity        = opacity;
    q.visible        = true;
    return q;
}

// Expand one splat quad corner (c in 0..3) into a VSOutput.
VSOutput emitCorner(SplatQuad q, uint c)
{
    // quad corners: (-1,-1)(1,-1)(-1,1)(1,1)
    const float2 corner    = float2((c & 1) ? 1.0 : -1.0, (c & 2) ? 1.0 : -1.0);
    const float2 offsetPix = corner.x * q.basis1 + corner.y * q.basis2;
    const float2 offsetNdc = offsetPix / (0.5 * uCamera.viewport) * q.centerClip.w;
    VSOutput     o;
    o.clipPos = q.centerClip + float4(offsetNdc, 0, 0);
    o.quadPos = corner * kSplatExtentStdDev;
    o.color   = float4(q.rgb, q.opacity);
    return o;
}

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

// Mesh-shader path: each workgroup expands kMeshGroup splats (in sorted order) into
// up to kMeshGroup quads (4 verts + 2 tris each). uDrawArgs[1] is the sort's visible
// (culled) count; groups past it emit nothing.
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
