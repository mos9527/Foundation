#include "ICommon.slang"

uniform UBO globalParams;
// vvv Cleared before dispatch
RWStructuredBuffer<Atomic<uint>> outTasksCounter;
RWStructuredBuffer<MeshletTaskWork> outTasks;

StructuredBuffer<GSInstance> instances;
ByteAddressBuffer primitive;

// Processes every instance and generates grouped (by kMeshWorkGroupSize) meshlets tasks
// for later culling.
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
    uint groups = alignUp(mesh.meshletCount, kMeshWorkGroupSize) / kMeshWorkGroupSize;
    uint taskIndex = outTasksCounter[0].add(groups);
    for (uint group = 0; group < groups; group++) {
        MeshletTaskWork task;
        task.instanceID = iid;
        task.numMeshlets = min(mesh.meshletCount - group * kMeshWorkGroupSize, kMeshWorkGroupSize);
        task.firstMeshlet = group * kMeshWorkGroupSize;
        outTasks[taskIndex + group] = task;
    }
}