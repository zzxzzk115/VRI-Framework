// Deferred lighting: fullscreen triangle sampling the gbuffer, one directional
// light + flat ambient, written to an HDR scene-color target.
import vsh;

[[vk::binding(0, 0)]] Texture2D uAlbedo;
[[vk::binding(1, 0)]] Texture2D uNormal;
[[vk::binding(2, 0)]] SamplerState uSampler;

struct VSOutput
{
    float4 clipPos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId: SV_VertexID)
{
    VSOutput o;
    o.uv      = float2((vertexId << 1) & 2, vertexId & 2);
    o.clipPos = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    const float4 albedo = uAlbedo.Sample(uSampler, input.uv);
    if (albedo.a < 0.5)
    {
        return float4(0.02, 0.02, 0.03, 1.0); // background
    }

    const float3 n = normalize(uNormal.Sample(uSampler, input.uv).xyz * 2.0 - 1.0);
    const float3 l = normalize(float3(0.4, 0.9, 0.3));

    const float  nDotL = max(dot(n, l), 0.0);
    const float3 color = albedo.rgb * (nDotL * 2.5 + 0.15);
    return float4(color, 1.0);
}
