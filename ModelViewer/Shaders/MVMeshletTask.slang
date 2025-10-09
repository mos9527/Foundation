#include "MVMeshlet.h"

groupshared MeshletPayload gsPayload;
// https://gpuopen.com/learn/mesh_shaders/mesh_shaders-optimization_and_best_practices/
// Each WG of task shader processes up to kMaxTaskShaderWork meshlets, and each
// thread in the WG processes one meshlet
[shader("amplification")]
[numthreads(kMaxTaskShaderWork, 1, 1)]
void meshletTask(uint meshlet: SV_GroupIndex, uint2 groupID: SV_GroupID) {
    uint count = counter[0].load();
    uint dispatchIndex = dot(groupID, uint2(kMaxTaskShaderWork, 1));
    bool visible = false;
    // Note to self: do NOT branch (inc. early return) with a memory barrier!! i.e. GroupMemoryBarrierWithGroupSync();
    // Initialize payload
    gsPayload.meshletCount = 0;
    GroupMemoryBarrierWithGroupSync();
    if (dispatchIndex < count) {
        MeshletTaskParams params = tasks[dispatchIndex];
        gsPayload.instanceID = params.instanceID;
        gsPayload.vertexRawOffset = params.vertexRawOffset;
        gsPayload.meshletVerticesRawOffset = params.meshletVerticesRawOffset;
        gsPayload.meshletTrianglesRawOffset = params.meshletTrianglesRawOffset;
        visible = meshlet < params.meshletCount;
        // TODO: Actual Culling
        if (visible) {
            uint gsPayloadIndex;
            InterlockedAdd(gsPayload.meshletCount, 1, gsPayloadIndex);
            gsPayload.meshletRawOffsets[gsPayloadIndex] = params.meshletRawOffset + meshlet * sizeof(MeshMeshlet);
        }
    }
    GroupMemoryBarrierWithGroupSync();    
    DispatchMesh(gsPayload.meshletCount, 1, 1, gsPayload);
}
