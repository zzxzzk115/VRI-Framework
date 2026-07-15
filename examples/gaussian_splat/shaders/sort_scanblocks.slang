// Sort stage 4: exclusive-scan the 256 block sums into block offsets.
#include "sort_common.slangh"

groupshared uint gScan[kBlockSize];

[shader("compute")]
[numthreads(256, 1, 1)]
void computeMain(uint3 gtid: SV_GroupThreadID)
{
    const uint local = gtid.x;
    const uint self  = uBlockSum[local];
    gScan[local]     = self;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1; offset < kBlockSize; offset <<= 1)
    {
        const uint v = (local >= offset) ? gScan[local - offset] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gScan[local] += v;
        GroupMemoryBarrierWithGroupSync();
    }
    uBlockOffset[local] = gScan[local] - self;
}
