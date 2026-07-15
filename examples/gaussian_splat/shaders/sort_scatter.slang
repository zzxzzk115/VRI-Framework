// Sort stage 6: per splat, pos = atomicAdd(offset[bucket]); sorted[pos] = i.
#include "sort_common.slangh"

[shader("compute")]
[numthreads(256, 1, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= uP.count)
        return;
    const uint b = uBucket[i];
    if (b == kCulled)
        return; // culled splats are not placed; the draw count excludes them
    uint pos;
    InterlockedAdd(uOffset[b], 1u, pos);
    uSorted[pos] = i;
}
