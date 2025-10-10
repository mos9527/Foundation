#include "MVMeshlet.h"
import "Math/Quaternion";
groupshared MeshletPayload shared;
// https://gpuopen.com/learn/mesh_shaders/mesh_shaders-optimization_and_best_practices/
// Each WG of task shader processes up to kMaxTaskShaderWork meshlets from the same Instance,
// and each thread in the WG processes one meshlet.
[shader("amplification")]
[numthreads(kMaxTaskShaderWork, 1, 1)]
void meshletTask(uint meshletIndex: SV_GroupIndex, uint2 groupID: SV_GroupID) {
    SceneParams scene = sceneShared.Load<SceneParams>(sceneParamsOffset);
    uint count = counter[0].load();
    uint dispatchIndex = dot(groupID, uint2(kMaxTaskShaderWork, 1));
    bool visible = true;
    // Note to self: do NOT branch (inc. early return) with a memory barrier!! i.e. GroupMemoryBarrierWithGroupSync();
    // Initialize payload
    shared.meshletCount = 0;
    GroupMemoryBarrierWithGroupSync();
    if (dispatchIndex < count) {
        MeshletTaskParams params = tasks[dispatchIndex];
        shared.instanceID = params.instanceID;
        shared.meshletRawOffset = params.meshletRawOffset;
        shared.vertexRawOffset = params.vertexRawOffset;
        shared.meshletVerticesRawOffset = params.meshletVerticesRawOffset;
        shared.meshletTrianglesRawOffset = params.meshletTrianglesRawOffset;
        visible = meshletIndex < params.meshletCount;
        if (visible) {
            uint32_t meshletRawOffset = params.meshletRawOffset + meshletIndex * sizeof(MeshMeshlet);
            SceneInstanceData instance = sceneInstance.Load<SceneInstanceData>(params.instanceID * sizeof(SceneInstanceData));
            MeshMeshlet meshlet = sceneConst.Load<MeshMeshlet>(meshletRawOffset);
            // vvv Cone culling
            float3 eye = scene.culling.cameraPosition;
            float3 apex = meshlet.coneApex + instance.t;
            float3 axis = instance.q.rotate(meshlet.coneAxisAngle.xyz);
            float angleCos = meshlet.coneAxisAngle.w;
            float3 toApex = normalize(apex - eye);
            visible &= dot(toApex, axis) <= angleCos;
            // vvv Sphere culling
            if (visible) {
                float4 frustum = scene.culling.frustum; // [left xy -> ac] [top zw -> bc]
                // See @ref Camera::GetCullParams
                // pLeft -> ax+cz = 0, pRight -> -ax+cz=0
                // pTop -> by+cz=0, pBottom -> -by+cz=0
                // Solve for left&right
                // -> dot(left, p) >= -r && dot(right, p) >= -r
                // -> ax+cz >= -r && -ax+cz >= -r
                // -> cz >= -r + |ax|
                // Same goes for top&bottom
                // -> cz >= -r + |by|
                float3 center = meshlet.sphereCenterRadius.xyz;
                center = instance.q.rotate(center);
                center += instance.t;
                center = mul(scene.culling.viewMatrix, float4(center, 1.0)).xyz;
                // Performed in View Space
                float r = meshlet.sphereCenterRadius.w;
                float ax = abs(frustum.x * center.x);
                float by = abs(frustum.z * center.y);
                float cz = frustum.y * center.z;
                float dz = frustum.w * center.z;
                visible &= (cz >= -r + ax);
                visible &= (dz >= -r + by);
            }
            if (visible) {
                uint sharedIndex;
                InterlockedAdd(shared.meshletCount, 1, sharedIndex);
                shared.meshletIndices[sharedIndex] = meshletIndex;
            }

        }
    }
    GroupMemoryBarrierWithGroupSync();    
    DispatchMesh(shared.meshletCount, 1, 1, shared);
}
