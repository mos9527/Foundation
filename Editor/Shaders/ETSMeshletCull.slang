#include "EMeshletCommon.slang"

uniform UBO globalParams;
RWStructuredBuffer<Atomic<uint>> counter;
RWStructuredBuffer<MeshletTaskWork> tasks;
StructuredBuffer<GSInstance> instances;
ByteAddressBuffer primitive;

float projErrorSimple(FLODGroup lodGroup) {
    float3 center = mul(globalParams.view, float4(lodGroup.center, 1.0)).xyz;
    float r = lodGroup.radius;
    float d = max(length(center) - r, globalParams.zNear);
    return lodGroup.error / d * abs(globalParams.proj[1][1]) * 0.5f;
}

groupshared MeshletGroupShared shared;

// Each WG of task shader processes up to kMaxTaskShaderWork meshlets from the same Instance,
// and each thread in the WG processes one meshlet.
[shader("amplification")]
[numthreads(kTaskShaderWG, 1, 1)]
void main(uint groupIndex: SV_GroupIndex, uint2 groupID: SV_GroupID) {
    uint count = counter[0].load();
    uint dispatchIndex = dot(groupID, uint2(kTaskShaderWG, 1)); // ID of this group, flattened
    if (dispatchIndex >= count)
        return; // Group inactive. Should NOT happen.    
    MeshletTaskWork work = tasks[dispatchIndex];
    GSInstance inst = instances[work.instanceID];
    GSMesh mesh = primitive.Load<GSMesh>(inst.data[0]);
    uint32_t meshletID = work.firstMeshlet + groupIndex;
    // Init
    shared.instanceID = work.instanceID;
    shared.meshletCount = 0;
    GroupMemoryBarrierWithGroupSync();
    bool culled = true;
    if (meshletID < mesh.meshletCount) {
        FMeshlet meshlet = primitive.Load<FMeshlet>(mesh.meshletOffset + meshletID * sizeof(FMeshlet));
        // View-dependent culling
        FLODGroup selfGroup = primitive.Load<FLODGroup>(mesh.groupOffset + meshlet.group * sizeof(FLODGroup));
        float selfError = projErrorSimple(selfGroup);
        // Remember - we take the upper_bound of the acceptable clusters
        // where it's the *first* LOD that exceeds the error threshold
        culled = selfError <= globalParams.lodThreshold;
        if (meshlet.refined != ~0u){
            FLODGroup refGroup = primitive.Load<FLODGroup>(mesh.groupOffset + meshlet.refined * sizeof(FLODGroup));
            float refError = projErrorSimple(refGroup);
            // The refined ones exceeds it too - impossible unless it's closer to
            // the actual upper bound. So cull ourselves.
            culled |= refError > globalParams.lodThreshold;
        }
    }
    if (!culled){
        // Output the visible meshlet
        // TODO: Wave intrinsics. See // https://gpuopen.com/learn/mesh_shaders/mesh_shaders-optimization_and_best_practices/
        uint sharedCount;
        InterlockedAdd(shared.meshletCount,1,sharedCount);
        shared.meshletIndices[sharedCount] = meshletID;
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(shared.meshletCount,1,1,shared);
}