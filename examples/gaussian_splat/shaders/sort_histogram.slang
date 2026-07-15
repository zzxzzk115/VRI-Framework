// Sort stage 2: per splat, bucket = quantized depth; count into the histogram.
#include "sort_common.slangh"

[shader("compute")]
[numthreads(256, 1, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= uP.count)
        return;
    const uint b = depthBucket(i);
    uBucket[i]   = b;
    uint prev;
    InterlockedAdd(uHisto[b], 1u, prev);
}
