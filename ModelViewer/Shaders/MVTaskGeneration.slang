import "Common/Definitions";
import "Math/Bits";

[[vk::push_constant]] SceneParams scene;

RWStructuredBuffer<MeshletTaskParams> tasks;
RWStructuredBuffer<Atomic<uint>> counter;
ByteAddressBuffer sceneInstance;
ByteAddressBuffer sceneConst;

[shader("compute")]
[numthreads(64, 1, 1)]
void taskGeneration(uint instanceID: SV_DispatchThreadID) {
    if (instanceID >= scene.instanceCount)
        return; // Out of range
    SceneInstanceData instance = sceneInstance.Load<SceneInstanceData>(instanceID * sizeof(SceneInstanceData));
    if (instance.meshAllocationRawOffsetPP == 0)
        return;
    SceneMeshAllocation mesh = sceneConst.Load<SceneMeshAllocation>(instance.meshAllocationRawOffsetPP - 1);
    // Just use LOD 0 for now
    // TODO: Corase culling and LOD selection
    SceneMeshLodAllocation lod = mesh.lods[0];
    // Generate tasks for each meshlet
    // Each task shader _group_ dispatch processes at most kMaxTaskShaderWork meshlets
    // and each task shader thread submits one mesh shader work
    uint groups = alignUp(lod.meshletCount, kMaxTaskShaderWork) / kMaxTaskShaderWork;
    uint taskIndex = counter[0].add(groups);
    for (uint group = 0; group < groups; group++) {

        MeshletTaskParams task;
        task.instanceID = instanceID;
        task.vertexRawOffset = mesh.vertexRawOffset;
        task.meshletCount = min(lod.meshletCount - group * kMaxTaskShaderWork, kMaxTaskShaderWork);
        task.meshletRawOffset = lod.meshletRawOffset + group * kMaxTaskShaderWork * sizeof(MeshMeshlet);
        task.meshletVerticesRawOffset = lod.meshletVerticesRawOffset;
        task.meshletTrianglesRawOffset = lod.meshletTrianglesRawOffset;        
        tasks[taskIndex + group] = task;
    }
}