// Present: copy the accumulated splat color straight to the swapchain. 3DGS
// colors are display-referred (SH DC -> 0.5 + C0*dc is already the sRGB display
// value, trained against sRGB images), so - like NVIDIA vk_gaussian_splatting -
// NO tonemapping or gamma is applied; the premultiplied-over result is output
// as-is onto a UNORM target.
import vsh;

[[vk::binding(0, 0)]] Texture2D    uSceneColor;
[[vk::binding(1, 0)]] SamplerState uSampler;

struct VSOutput
{
    float4 clipPos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")] VSOutput vertexMain(uint vertexId : SV_VertexID) {
    VSOutput o;
    o.uv      = float2((vertexId << 1) & 2, vertexId & 2);
    o.clipPos = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

    [shader("fragment")] float4 fragmentMain(VSOutput i) : SV_Target
{
    return float4(uSceneColor.Sample(uSampler, i.uv).rgb, 1.0);
}
