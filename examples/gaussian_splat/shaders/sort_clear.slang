// Sort stage 1: zero the 65536-bucket histogram; thread 0 seeds the indirect
// draw args (4 verts/quad, 0 instances - histogram counts visible splats in).
#include "sort_common.slangh"

[shader("compute")]
[numthreads(256, 1, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x < kBuckets)
        uHisto[tid.x] = 0;
    if (tid.x == 0)
    {
        uDrawArgs[0] = 4; // vertexNum (triangle-strip quad)
        uDrawArgs[1] = 0; // instanceNum (visible count, filled by histogram)
        uDrawArgs[2] = 0; // baseVertex
        uDrawArgs[3] = 0; // baseInstance
    }
}
