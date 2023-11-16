#include "../Common.hlsli"

groupshared uint s_instances[THREADS_PER_WAVE]; // splat instance lists for each LOD
groupshared uint s_meshletGroupOffsets[LOD_COUNT];
// s_lod* indexes into s_instances
groupshared uint s_lodCounts[LOD_COUNT]; // No. of instances in each LOD 
groupshared uint s_lodOffsets[LOD_COUNT];

/* - Every AS runs on a single wave
   - Every thread of AS processes one instance of geometry
*/
[RootSignature(ROOT_SIG)]
[NumThreads(THREADS_PER_WAVE, 1, 1)]
void main(uint dtid : SV_DispatchThreadID, uint gtid : SV_GroupThreadID, uint gid : SV_GroupID)
{   
    bool visible = dtid < g_dispatchData.dispatch_count;
    AS2MSPayload payload;
    if (visible)
    {
        uint index = WavePrefixCountBits(visible);
        payload.indices[index] = dtid;
    }
    DispatchMesh(visible, 1, 1, payload);
} 