// Desktop composite for simulation mode: the 2-layer eye target shown
// side-by-side in the window (left half = layer 0, right half = layer 1).
import vsh;

[[vk::binding(0, 0)]] Texture2DArray uEyes;
[[vk::binding(1, 0)]] SamplerState uSampler;

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
    const float layer = input.uv.x < 0.5 ? 0.0 : 1.0;
    const float2 eyeUv = float2(frac(input.uv.x * 2.0), input.uv.y);
    return uEyes.Sample(uSampler, float3(eyeUv, layer));
}
