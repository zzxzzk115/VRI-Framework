// Sort stage 1: zero the 65536-bucket histogram.
#include "sort_common.slangh"

[shader("compute")]
[numthreads(256, 1, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x < kBuckets)
        uHisto[tid.x] = 0;
}
