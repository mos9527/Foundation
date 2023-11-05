#include "../Common.hlsli"
template<typename T>
T read_buffer(uint geo_handle, uint offset, uint index)
{
    ByteAddressBuffer geoBuffer = ResourceDescriptorHeap[geo_handle];
    return geoBuffer.Load <T> (offset + index * sizeof(T));
}
uint3 get_primitive(uint geo_handle)
{
    
}
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
    meshlet m = read_buffer<meshlet>(h0.heap_handle, lod0.meshlet_offset, meshlet_index);
    
    SetMeshOutputCounts(m.vertex_count, m.triangle_count);
    if (gtid < m.vertex_count)
    {
        verts[gtid] = triangles_unpack()
    }

    
}