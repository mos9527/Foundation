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
    // Find the LOD to which this threadgroup is assigned.
    // Each wave does this independently to avoid groupshared memory & sync.
    uint offsetCheck = 0;
    uint laneIndex = gtid % WaveGetLaneCount();

    if (laneIndex <= LOD_COUNT)
    {
        offsetCheck = WaveActiveCountBits(gid >= payload.s_meshletGroupOffsets[laneIndex]) - 1;
    }
    uint lodIndex = WaveReadLaneFirst(offsetCheck);
    StructuredBuffer<GeometryData> geobuffer = ResourceDescriptorHeap[g_bufferIndices.geometryIndexBufferIndex];
    StructuredBuffer<InstanceData> instances = ResourceDescriptorHeap[g_bufferIndices.instanceBufferIndex];
    uint lodOffset = payload.s_lodOffsets[lodIndex];    
    uint instance = payload.s_instances[lodOffset];
    uint meshletIndex = gid - lodOffset;    
    InstanceData instanceData = instances.Load(instance);
    GeometryData geometryData = geobuffer.Load(instanceData.geometry_index);
    GeometryGPULod lodData = geometryData.lods[lodIndex];
    
    ByteAddressBuffer geo = ResourceDescriptorHeap[geometryData.heap_index];
    meshlet m = geo.Load<meshlet>(lodData.meshlet_offset + meshletIndex);
    
    SetMeshOutputCounts(m.vertex_count, m.triangle_count);
    
    if (gtid < m.vertex_count)
    {
        uint vertex = geo.Load<float>(lodData.meshlet_vertices_offset + gtid);
        verts[gtid].position = geo.Load<float4>(geometryData.position_offset + vertex);
        verts[gtid].normal = geo.Load<float4>(geometryData.normal_offset + vertex);
        verts[gtid].tangent = geo.Load<float4>(geometryData.tangent_offset + vertex);
        verts[gtid].uv = geo.Load<float4>(geometryData.uv_offset + vertex);        
    }
    
    if (gtid < m.triangle_count)
    {
        uint tri = geo.Load<float>(lodData.meshlet_triangles_offset + gtid);        
        tris[gtid] = triangles_unpack(tri);
    }
}