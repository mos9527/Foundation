#include "../Common.hlsli"

[RootSignature(ROOT_SIG)]
[NumThreads(THREADS_PER_WAVE, 1, 1)] // we want AS to be ran on a single wave so thread syncs are no longer necessary
void main(uint dtid : SV_DispatchThreadID, uint gtid : SV_GroupThreadID, uint gid : SV_GroupID)
{   
    StructuredBuffer<GeometryGPUData> handles = ResourceDescriptorHeap[g_constants.geometryHandleBufferIndex];
    as_ms_payload payload;
    payload.null = dtid;
    GeometryGPUData h0 = handles[0];
    uint count = h0.lods[0].meshlet_count;    
    DispatchMesh(ceil(count / MS_GROUP_SIZE), 1, 1, payload);
}