// PBR (metallic-roughness) mesh shader in Slang, authored for vshadersystem.
//
// This shader is compiled into a variant LIBRARY (mesh.vshlib) by `vshaderc build`, which
// bakes every permutation of the VTX_HAS_* keywords below. At runtime the framework selects
// the variant matching a mesh's actual vertex layout (vrf::ShaderKeywordsForVertexLayout),
// so a mesh without normals/tangents/uvs uses a shader with no such inputs - the framework
// does NOT assume a fixed vertex.
//
// The framework does NOT dictate shading - this is the example's own analytic PBR
// (Cook-Torrance BRDF, one directional light + a flat ambient term, tangent-space normal
// mapping when a mesh has tangents+uvs). VRI is Y-up, so NO proj Y flip is applied on the CPU.
//
// Vertex input locations are assigned by struct declaration order (canonical order:
// position, normal, tangent, color, texCoord0), matching vrf::MakeVertexLayout so a compact
// buffer (present attributes only) lines up with VRI's positional attribute matching.
//
// Regenerate the committed blob offline (opt-in; no build-time toolchain needed otherwise):
//   vshaderc build --shader_root examples/model_viewer/shaders -o mesh.vshlib --no-wgsl
//   (then embed mesh.vshlib as the byte array in mesh_vshlib.h)

import vsh;

// Vertex-attribute presence keywords. `permute global` => every combination is baked into the
// library; the framework sets these from the mesh's GpuVertexLayout.
[VshKeyword("VTX_HAS_NORMAL", "bool", "permute", "global")]
[VshKeyword("VTX_HAS_TANGENT", "bool", "permute", "global")]
[VshKeyword("VTX_HAS_UV0", "bool", "permute", "global")]
[VshKeyword("VTX_HAS_COLOR", "bool", "permute", "global")]
void __vsh_meta() {}

// vshaderc defines the bool keywords as 0/1; guard for editors/standalone slangc too.
#ifndef VTX_HAS_NORMAL
#define VTX_HAS_NORMAL 0
#endif
#ifndef VTX_HAS_TANGENT
#define VTX_HAS_TANGENT 0
#endif
#ifndef VTX_HAS_UV0
#define VTX_HAS_UV0 0
#endif
#ifndef VTX_HAS_COLOR
#define VTX_HAS_COLOR 0
#endif

struct VSInput
{
    float3 position : POSITION;
#if VTX_HAS_NORMAL
    float3 normal : NORMAL;
#endif
#if VTX_HAS_TANGENT
    float4 tangent : TANGENT; // xyz + handedness (w)
#endif
#if VTX_HAS_COLOR
    float4 color : COLOR0;
#endif
#if VTX_HAS_UV0
    float2 texCoord : TEXCOORD0;
#endif
};

struct VSOutput
{
    float4 clipPos  : SV_Position;
    float3 worldPos : POSITION0;
#if VTX_HAS_NORMAL
    float3 normal : NORMAL;
#endif
#if VTX_HAS_TANGENT
    float4 tangent : TANGENT; // world-space xyz + handedness (w)
#endif
#if VTX_HAS_UV0
    float2 texCoord : TEXCOORD0;
#endif
#if VTX_HAS_COLOR
    float4 color : COLOR0;
#endif
};

struct PushConstants
{
    float4x4 mvp; // clip-space transform (column-major: matches glm; vshaderc >= 1.0.1 defaults column-major)
    float4   modelCol0;  // model 3x3 column 0 (xyz) + translation.x (w)
    float4   modelCol1;  // column 1 (xyz) + translation.y (w)
    float4   modelCol2;  // column 2 (xyz) + translation.z (w)
    float4   camera;     // camera position (xyz) + debug-view mode (w, cast to int)
};

// Debug views (camera.w): 0 = full PBR, then unlit channel previews.
static const int kDebugPBR       = 0;
static const int kDebugBaseColor = 1;
static const int kDebugMetallic  = 2;
static const int kDebugRoughness = 3;
static const int kDebugNormal    = 4;
static const int kDebugOcclusion = 5;
static const int kDebugEmissive  = 6;
static const int kDebugTexCoord  = 7;
[[vk::push_constant]] PushConstants pc;

struct MaterialUBO
{
    float4 baseColorFactor;
    float4 factors;        // x=metallic, y=roughness, z=occlusionStrength, w=alphaCutoff
    float4 emissiveFactor; // xyz
};
// Pin the D3D12 register to b0 so the push constant (which Slang otherwise puts at b0) moves to
// b1, matching the pipeline layout - two root parameters at b0 fail D3D12SerializeRootSignature.
// [[vk::binding]] still drives the Vulkan (SPIR-V) binding independently.
[[vk::binding(0, 0)]] ConstantBuffer<MaterialUBO> uMaterial : register(b0, space0);
[[vk::binding(1, 0)]] Texture2D uBaseColor;
[[vk::binding(2, 0)]] Texture2D uMetallicRoughness;
[[vk::binding(3, 0)]] Texture2D uEmissive;
[[vk::binding(4, 0)]] Texture2D uOcclusion;
[[vk::binding(5, 0)]] Texture2D uNormal;   // tangent-space; flat (0.5,0.5,1) when absent
[[vk::binding(6, 0)]] SamplerState uSampler;

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput o;
    o.clipPos = mul(pc.mvp, float4(input.position, 1.0));

    // model columns dotted with the vertex components (column-major; avoids ctor conventions)
    const float3 worldRotPos = pc.modelCol0.xyz * input.position.x + pc.modelCol1.xyz * input.position.y +
                               pc.modelCol2.xyz * input.position.z;
    o.worldPos = worldRotPos + float3(pc.modelCol0.w, pc.modelCol1.w, pc.modelCol2.w);
#if VTX_HAS_NORMAL
    o.normal = pc.modelCol0.xyz * input.normal.x + pc.modelCol1.xyz * input.normal.y +
               pc.modelCol2.xyz * input.normal.z;
#endif
#if VTX_HAS_TANGENT
    const float3 worldTangent = pc.modelCol0.xyz * input.tangent.x + pc.modelCol1.xyz * input.tangent.y +
                                pc.modelCol2.xyz * input.tangent.z;
    o.tangent = float4(worldTangent, input.tangent.w);
#endif
#if VTX_HAS_UV0
    o.texCoord = input.texCoord;
#endif
#if VTX_HAS_COLOR
    o.color = input.color;
#endif
    return o;
}

static const float PI = 3.14159265359;

float3 fresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float distributionGGX(float3 n, float3 h, float roughness)
{
    const float a      = roughness * roughness;
    const float a2     = a * a;
    const float nDotH  = max(dot(n, h), 0.0);
    const float denom  = (nDotH * nDotH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-5);
}

float geometrySchlickGGX(float nDotV, float roughness)
{
    const float r = roughness + 1.0;
    const float k = (r * r) / 8.0;
    return nDotV / (nDotV * (1.0 - k) + k);
}

float geometrySmith(float3 n, float3 v, float3 l, float roughness)
{
    return geometrySchlickGGX(max(dot(n, v), 0.0), roughness) * geometrySchlickGGX(max(dot(n, l), 0.0), roughness);
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    // Texture coordinates only exist when the mesh has UV0; otherwise material factors carry.
#if VTX_HAS_UV0
    const float2 uv = input.texCoord;
    const float4 baseSample = uBaseColor.Sample(uSampler, uv);
    const float3 mr         = uMetallicRoughness.Sample(uSampler, uv).rgb;
    const float  aoSample   = uOcclusion.Sample(uSampler, uv).r;
    const float3 emiSample  = uEmissive.Sample(uSampler, uv).rgb;
#else
    const float2 uv = float2(0.0, 0.0);
    const float4 baseSample = float4(1.0, 1.0, 1.0, 1.0);
    const float3 mr         = float3(1.0, 1.0, 1.0);
    const float  aoSample   = 1.0;
    const float3 emiSample  = float3(1.0, 1.0, 1.0);
#endif

    // Base color (glTF baseColor textures are sRGB; approximate the decode to linear).
    float3 albedo = pow(baseSample.rgb, 2.2) * uMaterial.baseColorFactor.rgb;
#if VTX_HAS_COLOR
    albedo *= input.color.rgb; // vertex color modulates
#endif

    // Metallic-roughness: glTF packs roughness in G, metallic in B.
    const float  metallic  = mr.b * uMaterial.factors.x;
    const float  roughness = clamp(mr.g * uMaterial.factors.y, 0.04, 1.0);
    const float  ao        = lerp(1.0, aoSample, uMaterial.factors.z);
    const float3 emissive  = pow(emiSample, 2.2) * uMaterial.emissiveFactor.rgb;

    // Geometric normal: the interpolated vertex normal, or a face normal from screen-space
    // derivatives when the mesh has none.
#if VTX_HAS_NORMAL
    const float3 geomN = normalize(input.normal);
#else
    const float3 geomN = normalize(cross(ddx(input.worldPos), ddy(input.worldPos)));
#endif

    // Tangent-space normal mapping needs both a tangent frame and uvs.
#if VTX_HAS_TANGENT && VTX_HAS_UV0
    float3      tangent = input.tangent.xyz - geomN * dot(geomN, input.tangent.xyz); // re-orthonormalize
    const float tanLen  = length(tangent);
    tangent             = tanLen > 1e-5 ? tangent / tanLen : float3(1.0, 0.0, 0.0);
    const float3 bitangent = cross(geomN, tangent) * input.tangent.w; // handedness
    const float3 nSample   = uNormal.Sample(uSampler, uv).xyz * 2.0 - 1.0;
    const float3 n         = normalize(nSample.x * tangent + nSample.y * bitangent + nSample.z * geomN);
#else
    const float3 n = geomN;
#endif

    // Debug views: unlit channel previews for inspecting the material inputs.
    const int mode = int(pc.camera.w + 0.5);
    if (mode == kDebugBaseColor)
        return float4(pow(albedo, 1.0 / 2.2), 1.0);
    if (mode == kDebugMetallic)
        return float4(metallic, metallic, metallic, 1.0);
    if (mode == kDebugRoughness)
        return float4(roughness, roughness, roughness, 1.0);
    if (mode == kDebugNormal)
        return float4(n * 0.5 + 0.5, 1.0);
    if (mode == kDebugOcclusion)
        return float4(ao, ao, ao, 1.0);
    if (mode == kDebugEmissive)
        return float4(pow(emissive, 1.0 / 2.2), 1.0);
    if (mode == kDebugTexCoord)
        return float4(uv, 0.0, 1.0);

    const float3 v = normalize(pc.camera.xyz - input.worldPos);
    const float3 l = normalize(float3(0.4, 0.9, 0.3)); // one directional light
    const float3 h = normalize(v + l);
    const float3 radiance = float3(3.0, 3.0, 3.0); // light color * intensity

    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    const float  ndf = distributionGGX(n, h, roughness);
    const float  g   = geometrySmith(n, v, l, roughness);
    const float3 f   = fresnelSchlick(max(dot(h, v), 0.0), f0);

    const float  nDotL    = max(dot(n, l), 0.0);
    const float  nDotV    = max(dot(n, v), 0.0);
    const float3 specular = (ndf * g * f) / max(4.0 * nDotV * nDotL, 1e-4);

    const float3 kd = (float3(1.0, 1.0, 1.0) - f) * (1.0 - metallic);
    const float3 lo = (kd * albedo / PI + specular) * radiance * nDotL;

    const float3 ambient = albedo * ao * 0.3; // flat ambient term
    float3       color   = lo + ambient + emissive;

    color = color / (color + float3(1.0, 1.0, 1.0)); // Reinhard tonemap
    color = pow(color, 1.0 / 2.2);                   // linear -> sRGB for the UNORM target
    return float4(color, baseSample.a * uMaterial.baseColorFactor.a);
}
