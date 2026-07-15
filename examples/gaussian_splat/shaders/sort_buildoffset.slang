// Sort stage 5: offset[b] = localScan[b] + blockOffset[b >> 8].
#include "sort_common.slangh"

[shader("compute")]
[numthreads(256, 1, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID)
{
    const uint b = tid.x;
    if (b < kBuckets)
        uOffset[b] = uHisto[b] + uBlockOffset[b >> 8];
}
