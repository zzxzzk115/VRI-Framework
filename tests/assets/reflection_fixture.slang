// The source of reflection_fixture.vshlib (committed beside it), cooked with vshaderc v1.2.0:
//
//   vshaderc build --shader_root tests/assets --out tests/assets/reflection_fixture.vshlib \
//                  --targets spirv reflection_fixture.slang
//
// It exists so test_shader_reflection.cpp can assert on a binding table that is small enough to
// write out by hand, and it covers one descriptor of each kind the mirror maps: a sampled texture,
// a separate sampler, a storage image, a constant buffer and a read-only structured buffer, plus
// a [numthreads] the mirror carries as localSize. The keyword gives it two variants, which is what
// makes the "reflection is per variant, and the cooker dead-strips" property testable: t_Extra is
// referenced only when EXTRA_TEXTURE is on.
import vsh;

[VshKeyword("EXTRA_TEXTURE", "bool", "permute", "global")]
void __vsh_meta() {}

#ifndef EXTRA_TEXTURE
#define EXTRA_TEXTURE 0
#endif

struct Params
{
    float4 scale;
    uint2  extent;
};

[[vk::binding(0, 0)]] ConstantBuffer<Params> u_Params;
[[vk::binding(1, 0)]] Texture2D              t_Source;
[[vk::binding(2, 0)]] SamplerState           s_Linear;
[[vk::binding(3, 0)]] StructuredBuffer<uint> b_Indices;
[[vk::binding(4, 0)]] [format("rgba32f")] RWTexture2D<float4> u_Dst;
// Gated the "wrong" way round on purpose: the BASE variant (every keyword 0) is the one that
// carries t_Extra. See test_shader_reflection.cpp - the cooker reflects the base variant only, so
// gating it the other way makes the extra binding invisible in every variant.
#if !EXTRA_TEXTURE
[[vk::binding(5, 0)]] Texture2D t_Extra;
#endif

[shader("compute")]
[numthreads(8, 4, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= u_Params.extent.x || tid.y >= u_Params.extent.y)
        return;

    const float2 uv = (float2(tid.xy) + 0.5f) / float2(u_Params.extent);
    float4       c  = t_Source.SampleLevel(s_Linear, uv, 0.0f) * u_Params.scale;
    c.w += float(b_Indices[tid.x]);
#if !EXTRA_TEXTURE
    c += t_Extra.SampleLevel(s_Linear, uv, 0.0f);
#endif
    u_Dst[tid.xy] = c;
}
