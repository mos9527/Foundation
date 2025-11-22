#include "EMeshletCommon.slang"

RWStructuredBuffer<Atomic<uint>> counter;
RWStructuredBuffer<MeshletTaskWork> tasks;
StructuredBuffer<GSInstance> instance;
ByteAddressBuffer primitive;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint tid: SV_DispatchThreadID) {
    if (tid > globalParams.numInstances)
        return;
    uint iid = tid + globalParams.firstInstance;
    GSInstance inst = instance[iid];
    // TODO: Culling instance
    // Would require AABB update on the CPU side. Not implemented yet.
    GSMesh mesh = primitive.Load<GSMesh>(inst.data[0]);
    // Not discrete. LOD0 is used for DAG lods.
    GSMeshLOD lod = mesh.lod[0];
    // Generate tasks for each meshlet. Not culling them now!
    // Task and mesh shader dispatch is one-to-one, or one-to-zero.
    // Each task shader _group_ dispatch processes at most kMaxTaskShaderWork meshlets
    // and each task shader thread submits one mesh shader work.
    uint groups = alignUp(lod.meshletCount, kTaskShaderWG) / kTaskShaderWG;
    uint taskIndex = counter[0].add(groups);
    for (uint group = 0; group < groups; group++) {
        MeshletTaskWork task;
        task.instanceID = iid;
        task.numMeshlets = min(lod.meshletCount - group * kTaskShaderWG, kTaskShaderWG);
        task.firstMeshlet = group * kTaskShaderWG;
        tasks[taskIndex + group] = task;
    }
}