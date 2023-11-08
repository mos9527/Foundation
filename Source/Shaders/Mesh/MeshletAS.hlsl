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
    ConstantBuffer<CameraData> camera = ResourceDescriptorHeap[g_bufferIndices.cameraIndex];
    StructuredBuffer<GeometryData> geobuffer = ResourceDescriptorHeap[g_bufferIndices.geometryIndexBufferIndex];
    
    s_instances[gtid] = 0;
    uint instance = dtid + g_dispatchData.dispatch_offset;
    uint lodLevel = LOD_COUNT; // LOD_COUNT -> culled
    uint lodOffset = 0;
    
    InstanceData instanceData = (InstanceData) 0;
    GeometryData geometryData = (GeometryData) 0;
    // Cull & compact instances
    if (instance < g_dispatchData.dispatch_count)
    {
        StructuredBuffer<InstanceData> instances = ResourceDescriptorHeap[g_bufferIndices.instanceBufferIndex];
        instanceData = instances.Load(instance);        
        if (IsVisible(camera, instanceData.aabb_sphere_center_radius))
        {
            geometryData = geobuffer.Load(instanceData.geometry_index);
            float lod_ratio = ComputeLOD(camera, instanceData.aabb_sphere_center_radius);
            lodLevel = lod_ratio * (LOD_COUNT - 1);
        }        
    }
    // Count No. of instances per lod in lockstep
    for (uint i = 0; i < LOD_COUNT; i++)
    {
        bool lod_match = i == lodLevel;
        if (lod_match)
            lodOffset = WavePrefixCountBits(lod_match);
        s_lodCounts[i] = WaveActiveCountBits(lod_match);
    }
    // With the first LOD_COUNT thread, accumalate the offsets
    if (gtid < LOD_COUNT)
    {
        uint lodCount = s_lodCounts[gtid];
        s_lodOffsets[gtid + 1] = lodCount;     
        
        uint meshletCount = geometryData.lods[gtid].meshlet_count;
        s_meshletGroupOffsets[gtid + 1] = meshletCount;
    }
    if (gtid <= LOD_COUNT)
    {
        uint instanceCount = s_lodOffsets[gtid];
        s_lodOffsets[gtid] = instanceCount + WavePrefixSum(instanceCount);
        
        uint meshletCount = s_meshletGroupOffsets[gtid];
        s_meshletGroupOffsets[gtid] = meshletCount + WavePrefixSum(meshletCount);
    }
    // Prepare the linear instance buffer for MS
    uint totalMeshletCount = 0;
    if (lodLevel != LOD_COUNT)
    {
        totalMeshletCount = geometryData.lods[lodLevel].meshlet_count;
        totalMeshletCount = WaveActiveSum(totalMeshletCount);
        s_instances[s_lodOffsets[gtid] + lodOffset] = instance;
    }
    // Dispatch the MESH SHADER!!!!!!!
    AS2MSPayload payload;
    payload.s_instances = s_instances;    
    payload.s_lodCounts = s_lodCounts;
    payload.s_lodOffsets = s_lodOffsets;
    DispatchMesh(totalMeshletCount, 1, 1, payload);
}