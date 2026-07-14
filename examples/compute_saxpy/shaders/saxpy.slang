// Minimal compute shader: SAXPY (y = a*x + y) over two storage buffers.
// Enough to verify a ComputePipelineBuilder pipeline dispatches and writes a
// storage buffer end to end. `a` and the element count come through a push
// constant; one thread per element, 64-wide workgroups.
import vsh;

struct Params
{
    float a;
    uint  count;
};
[[vk::push_constant]] ConstantBuffer<Params> uParams;

[[vk::binding(0, 0)]] StructuredBuffer<float> uX;   // read-only input
[[vk::binding(1, 0)]] RWStructuredBuffer<float> uY; // in/out accumulator

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= uParams.count)
        return;
    uY[i] = uParams.a * uX[i] + uY[i];
}
