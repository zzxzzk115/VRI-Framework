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
    o.uv = float2((vertexId << 1) & 2, vertexId & 2);
    // VRI is Y-up (clip +Y = screen top). The source is a compute-written storage
    // texture in image orientation (row 0 = top), so map uv.y=0 -> clip +1 for an
    // identity row copy; the raster-style `uv * 2 - 1` would display it flipped.
    o.clipPos = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
    return o;
}

    [shader("fragment")] float4 fragmentMain(VSOutput i) : SV_Target
{
    return float4(uSceneColor.Sample(uSampler, i.uv).rgb, 1.0);
}
