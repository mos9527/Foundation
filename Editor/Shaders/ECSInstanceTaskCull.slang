#include "ICommon.slang"

uniform UBO globalParams;
RWStructuredBuffer<Atomic<uint>> counter;
RWStructuredBuffer<MeshletTaskWork> tasks;
StructuredBuffer<GSInstance> instances;
ByteAddressBuffer primitive;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint tid: SV_DispatchThreadID) {
    if (tid > globalParams.numInstances)
        return;
    uint iid = tid + globalParams.firstInstance;
    GSInstance inst = instances[iid];
    // TODO: Culling instance
    // Would require AABB update on the CPU side. Not implemented yet.
    GSMesh mesh = primitive.Load<GSMesh>(inst.meshOffset);
    // Generate tasks for each meshlet. Not culling them now!
    // Task and mesh shader dispatch is one-to-one, or one-to-zero.
    // Each task shader _group_ dispatch processes at most kMaxTaskShaderWork meshlets
    // and each task shader thread submits one mesh shader work.
    uint groups = alignUp(mesh.meshletCount, kMeshWorkGroupSize) / kMeshWorkGroupSize;
    uint taskIndex = counter[0].add(groups);
    for (uint group = 0; group < groups; group++) {
        MeshletTaskWork task;
        task.instanceID = iid;
        task.numMeshlets = min(mesh.meshletCount - group * kMeshWorkGroupSize, kMeshWorkGroupSize);
        task.firstMeshlet = group * kMeshWorkGroupSize;
        tasks[taskIndex + group] = task;
    }
}