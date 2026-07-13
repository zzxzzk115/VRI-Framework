// Tonemap: fullscreen Reinhard + gamma from the HDR scene color to the
// swapchain backbuffer.
import vsh;

[[vk::binding(0, 0)]] Texture2D uSceneColor;
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
    float3 color = uSceneColor.Sample(uSampler, input.uv).rgb;
    color        = color / (color + 1.0);  // Reinhard
    color        = pow(color, 1.0 / 2.2);  // linear -> sRGB
    return float4(color, 1.0);
}
