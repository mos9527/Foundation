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
    if (tid >= globalParams.instances.count)
        return;
    uint iid = tid + globalParams.instances.offset;
    GSInstance inst = instances[iid];
    // Only static meshes go through the meshlet pipeline. Curves and dynamic (CPU-updateable)
    // geometry are drawn elsewhere (dynamic geo via a dedicated vertex/index MDI draw), and carry
    // no DAG/meshlets, so skip them here.
    if (GSInstanceType(inst) != 0u || GSInstanceIsDynamic(inst))
        return;
    // TODO: Culling instance
    // Would require AABB update on the CPU side. Not implemented yet.
    GSMesh mesh = primitive.Load<GSMesh>(inst.resourceOffset);
    uint groups = alignUp(mesh.meshlets.count, kMeshWorkGroupSize) / kMeshWorkGroupSize;
    uint taskIndex = outTasksCounter[0].add(groups);
    for (uint group = 0; group < groups; group++) {
        MeshletTaskWork task;
        task.instanceID = iid;
        task.numMeshlets = min(mesh.meshlets.count - group * kMeshWorkGroupSize, kMeshWorkGroupSize);
        task.firstMeshlet = group * kMeshWorkGroupSize;
        outTasks[taskIndex + group] = task;
    }
    uint groupIndex = taskIndex + groups;
    groupIndex = WaveActiveMax(groupIndex);
    // Update dispatch count
    // We used to do this with another tiny CS - this will do too, and saves a barrier.
    // Same trick is used in meshlet culling CS as well.
    // As per VkPhysicalDeviceMeshShaderPropertiesEXT::maxTaskWorkGroupCount
    // min requirements are (65535,65535,65535)
    // Local size for task (and compute) shader is [kMeshWorkGroupSize,1 ,1]
    // We'll do [ceil(N/kMeshWorkGroupSize),min(N,kMeshWorkGroupSize),1] dispatches for N tasks - task shaders can read the
    // actual task count from the prepared count buffer
    if (WaveIsFirstLane()){
        uint groupX = (groupIndex + kMeshWorkGroupSize - 1) / kMeshWorkGroupSize;
        uint groupY = min(groupIndex, kMeshWorkGroupSize);
        uint groupZ = 1;
        InterlockedMax(outTasksDispatch[0].groupCountX, groupX);
        InterlockedMax(outTasksDispatch[0].groupCountY, groupY);
        outTasksDispatch[0].groupCountZ = groupZ; // const
    }
}