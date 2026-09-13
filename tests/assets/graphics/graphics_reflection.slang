// Cook with vshaderc v1.2.1:
// vshaderc build --shader_root tests/assets/graphics -o tests/assets/graphics_reflection.vshlib --targets spirv,wgsl
import vsh;

[VshKeyword("VTX_HAS_NORMAL", "bool", "permute", "global")]
[VshRenderState("cull", "front")]
[VshRenderState("depth_test", "off")]
[VshRenderState("depth_write", "off")]
[VshRenderState("depth_func", "greater")]
[VshRenderState("blend", "on")]
[VshRenderState("alpha_to_coverage", "on")]
void __vsh_meta() {}

#ifndef VTX_HAS_NORMAL
#define VTX_HAS_NORMAL 0
#endif

struct VertexInput
{
    float3 position : POSITION;
#if VTX_HAS_NORMAL
    float3 normal : NORMAL;
#endif
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color : COLOR;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    output.position = float4(input.position, 1.0);
#if VTX_HAS_NORMAL
    output.color = input.normal * 0.5 + 0.5;
#else
    output.color = float3(1.0, 0.0, 0.0);
#endif
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    return float4(input.color, 1.0);
}
