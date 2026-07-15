// Sort stage 2: per splat, frustum/size-cull; visible ones get bucket = quantized
// depth, count into the histogram, and increment the indirect draw instance count.
#include "sort_common.slangh"

[shader("compute")]
[numthreads(256, 1, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= uP.count)
        return;
    if (culled(i))
    {
        uBucket[i] = kCulled;
        return;
    }
    const uint b = depthBucket(i);
    uBucket[i]   = b;
    uint prev;
    InterlockedAdd(uHisto[b], 1u, prev);
    InterlockedAdd(uDrawArgs[1], 1u, prev); // VriDrawDesc.instanceNum
}
