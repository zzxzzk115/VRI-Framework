// Single-pass-stereo triangle: rendered once with multiview (viewMask 0b11)
// into a 2-layer eye target. SV_ViewID selects the eye - left red, right green
// - plus a small parallax nudge. Identical for a real XR swapchain image and
// the SimStereoRig's owned target.
import vsh;

struct VSOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

static const float2 kPositions[3] = { float2(0.0, -0.6), float2(0.6, 0.6), float2(-0.6, 0.6) };

[shader("vertex")]
VSOutput vertexMain(uint vid: SV_VertexID, uint viewID: SV_ViewID)
{
    VSOutput o;
    float    parallax = (viewID == 0) ? 0.05 : -0.05;
    o.position = float4(kPositions[vid] + float2(parallax, 0.0), 0.0, 1.0);
    o.color    = (viewID == 0) ? float3(1, 0.2, 0.2) : float3(0.2, 1, 0.2);
    return o;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    return float4(input.color, 1.0);
}
