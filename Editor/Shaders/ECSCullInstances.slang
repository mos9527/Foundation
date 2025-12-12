#include "ICommon.slang"

uniform UBO globalParams;
// vvv Cleared before dispatch
RWStructuredBuffer<Atomic<uint>> outTasksCounter;
RWStructuredBuffer<MeshletTaskWork> outTasks;

StructuredBuffer<GSInstance> instances;
ByteAddressBuffer primitive;

RWStructuredBuffer<MeshletTaskDispatch> outTasksDispatch;
// Processes every instance and generates grouped (by kMeshWorkGroupSize) meshlets tasks
// for later culling.
[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint tid: SV_DispatchThreadID, uint gid: SV_GroupIndex) {
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
    uint groupIndex = taskIndex + groups;
    // Update dispatch count
    // We used to do this with another tiny CS - this will do too, and saves a barrier.
    // Same trick is used in meshlet culling CS as well.
    // As per VkPhysicalDeviceMeshShaderPropertiesEXT::maxTaskWorkGroupCount
    // min requirements are (65535,65535,65535)
    // Local size for task (and compute) shader is [kMeshWorkGroupSize,1 ,1]
    // We'll do [ceil(N/kMeshWorkGroupSize),min(N,kMeshWorkGroupSize),1] dispatches for N tasks - task shaders can read the
    // actual task count from the prepared count buffer
    uint groupX = (groupIndex + kMeshWorkGroupSize - 1) / kMeshWorkGroupSize;
    uint groupY = min(groupIndex, kMeshWorkGroupSize);
    uint groupZ = 1;
    InterlockedMax(outTasksDispatch[0].groupCountX, groupX);
    InterlockedMax(outTasksDispatch[0].groupCountY, groupY);
    outTasksDispatch[0].groupCountZ = groupZ; // const
}