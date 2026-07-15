// Sort stage 6: per splat, pos = atomicAdd(offset[bucket]); sorted[pos] = i.
#include "sort_common.slangh"

[shader("compute")]
[numthreads(256, 1, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= uP.count)
        return;
    uint pos;
    InterlockedAdd(uOffset[uBucket[i]], 1u, pos);
    uSorted[pos] = i;
}
