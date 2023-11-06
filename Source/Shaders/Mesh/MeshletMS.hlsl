#include "../Common.hlsli"

[RootSignature(ROOT_SIG)]
[NumThreads(MS_GROUP_SIZE, 1, 1)]
[OutputTopology("triangle")]
void main(
    uint gtid : SV_GroupIndex,
    uint gid : SV_GroupID,
    in payload as_ms_payload payload,
    out vertices ms_ps_payload verts[MESHLET_MAX_VERTICES],
    out indices uint3 tris[MESHLET_MAX_PRIMITIVES])
{
    StructuredBuffer<GeometryGPUHandle> handles = ResourceDescriptorHeap[g_constants.geometryHandleBufferIndex];
    GeometryGPUHandle h0 = handles[0];
    GeometryGPULod lod0 = h0.lods[0];    
    uint meshlet_index = min(gtid, lod0.meshlet_count);
    
    SetMeshOutputCounts(0,0);
    verts[gtid] = (ms_ps_payload) 0;

    
}