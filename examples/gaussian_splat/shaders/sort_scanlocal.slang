// Sort stage 3: exclusive-scan each 256-bucket block (shared-mem Hillis-Steele);
// uHisto becomes the in-block exclusive prefix, uBlockSum gets each block total.
#include "sort_common.slangh"

groupshared uint gScan[kBlockSize];

[shader("compute")]
[numthreads(256, 1, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID, uint3 gtid: SV_GroupThreadID, uint3 gid: SV_GroupID)
{
    const uint local = gtid.x;
    const uint idx   = tid.x;
    const uint self  = (idx < kBuckets) ? uHisto[idx] : 0u;
    gScan[local]     = self;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1; offset < kBlockSize; offset <<= 1)
    {
        const uint v = (local >= offset) ? gScan[local - offset] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gScan[local] += v;
        GroupMemoryBarrierWithGroupSync();
    }
    const uint inclusive = gScan[local];
    if (idx < kBuckets)
        uHisto[idx] = inclusive - self;
    if (local == kBlockSize - 1)
        uBlockSum[gid.x] = inclusive;
}
