#include "../Common.hlsli"

[RootSignature(ROOT_SIG)]
[NumThreads(MS_GROUP_SIZE, 1, 1)]
[OutputTopology("triangle")]
void main(
    uint gtid : SV_GroupIndex,
    uint gid : SV_GroupID,
    in payload AS2MSPayload payload,
    out vertices Vertex verts[MESHLET_MAX_VERTICES],
    out indices uint3 tris[MESHLET_MAX_PRIMITIVES])
{
    ConstantBuffer<CameraData> camera = ResourceDescriptorHeap[g_bufferIndices.cameraIndex];
    StructuredBuffer<GeometryData> geobuffer = ResourceDescriptorHeap[g_bufferIndices.geometryIndexBufferIndex];
    uint meshlet = payload.indices[gid];
    
    SetMeshOutputCounts(0, 0);
    verts[gtid].position = float4(gtid, 0, 0, 0);
}