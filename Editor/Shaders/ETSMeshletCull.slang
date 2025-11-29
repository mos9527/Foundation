#include "EMeshletCommon.slangi"
[[SpecializationConstant]] int cullFlags;

uniform UBO globalParams;
[[vk_binding(1,0)]] StructuredBuffer<GSInstance> instances;
[[vk_binding(2,0)]] ByteAddressBuffer primitive;
[[vk_binding(3,0)]] RWStructuredBuffer<MeshletTaskWork> tasks;
[[vk_binding(4,0)]] RWStructuredBuffer<Atomic<uint>> counter;


float3 pointToView(float3 point, GSInstance inst, float4x4 view) {
    float3 worldPos = inst.rotation.rotate(point) * inst.scale + inst.transform;
    return mul(view, float4(worldPos, 1.0)).xyz;
}
bool frustumCull(float4 ijkl /* [left ac:ij] [top bc:kl] */, float3 center /* view */, float radius /* scaled */, float zNear) {

    float ix = abs(ijkl.x * center.x);
    float ky = abs(ijkl.z * center.y);
    float jz = ijkl.y * center.z;
    float lz = ijkl.w * center.z;
    return (center.z <= zNear) && (jz >= ix - radius) && (lz >= ky - radius);
}

float projErrorSimple(float error, float3 center /* view */, float radius /* scaled */, float P11, float zNear) {
    float d = max(length(center) - radius, zNear);
    return error / d * abs(P11) * 0.5f;
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
    GSMesh mesh = primitive.Load<GSMesh>(inst.meshOffset);
    uint32_t meshletID = work.firstMeshlet + groupIndex;
    // Init
    shared.instanceID = work.instanceID;
    shared.meshletCount = 0;
    GroupMemoryBarrierWithGroupSync();
    float scale = max(max(inst.scale.x,inst.scale.y),inst.scale.z);
    // Culling
    bool visible = false;
    FLODGroup group;
    float3 viewCenter;
    if (meshletID < mesh.meshletCount) {
        FMeshlet meshlet = primitive.Load<FMeshlet>(mesh.meshletOffset + meshletID * sizeof(FMeshlet));
        // View-dependent culling
        group = primitive.Load<FLODGroup>(mesh.groupOffset + meshlet.group * sizeof(FLODGroup));
        viewCenter = pointToView(group.center, inst, globalParams.view);
        float selfError = projErrorSimple(group.error, viewCenter, group.radius * scale, globalParams.proj[1][1], globalParams.zNear);
        // Remember - we take the upper_bound of the acceptable clusters
        // where it's the *first* LOD that exceeds the error threshold
        visible = selfError > globalParams.lodThreshold;
        if (meshlet.refined != ~0u){
            group = primitive.Load<FLODGroup>(mesh.groupOffset + meshlet.refined * sizeof(FLODGroup));
            viewCenter = pointToView(group.center, inst, globalParams.view);
            float refError = projErrorSimple(group.error, viewCenter, group.radius * scale, globalParams.proj[1][1], globalParams.zNear);
            // The refined ones exceeds it too - impossible unless it's closer to
            // the actual upper bound. So cull ourselves.
            visible &= refError <= globalParams.lodThreshold;
        }
    }
    if ((cullFlags & kCullFrustum) && visible){
        // Frustum cull
        visible &= frustumCull(globalParams.projPlanes, viewCenter, group.radius * scale, globalParams.zNear);
    }
    if (visible) {
        // Output the visible meshlet
        // TODO: Wave intrinsics. See // https://gpuopen.com/learn/mesh_shaders/mesh_shaders-optimization_and_best_practices/
        uint sharedCount;
        InterlockedAdd(shared.meshletCount,1,sharedCount);
        shared.meshletIndices[sharedCount] = meshletID;
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(shared.meshletCount,1,1,shared);
}