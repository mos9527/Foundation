#include "EMeshletCommon.slang"

uniform UBO globalParams;
[[vk_binding(1,0)]] RWStructuredBuffer<Atomic<uint>> counter;
[[vk_binding(2,0)]] RWStructuredBuffer<MeshletTaskWork> tasks;
[[vk_binding(3,0)]] StructuredBuffer<GSInstance> instances;
[[vk_binding(4,0)]] ByteAddressBuffer primitive;

groupshared float3 sharedScreenSpaceVerts[kTaskShaderWG];
// Each WG of mesh shader processes one meshlet
// Each thread in the WG processes zero, or more vertices/triangles
[outputtopology("triangle")]
[numthreads(kTaskShaderWG, 1, 1)]
void main(
    in payload MeshletGroupShared shared,
    in uint gid: SV_GroupID,
    in uint tid: SV_GroupIndex,
    OutputVertices<V2F, 96> verts,
    OutputIndices<uint3, 96> triangles
) {
    if (gid > shared.meshletCount)
        return;
    uint meshletID = shared.meshletIndices[gid];
    GSInstance inst = instances[shared.instanceID];
    GSMesh mesh = primitive.Load<GSMesh>(inst.data[0]);
    FMeshlet meshlet = primitive.Load<FMeshlet>(mesh.meshletOffset + meshletID * sizeof(FMeshlet));
    uint32_t vtxBase = mesh.meshletVtxOffset + meshlet.vtxOffset * sizeof(uint32_t);
    uint32_t vtxCount = meshlet.vtxCount;
    uint32_t triBase = mesh.meshletTriOffset + meshlet.triOffset * sizeof(uint8_t);
    uint32_t triCount = meshlet.triCount;
    SetMeshOutputCounts(vtxCount, triCount);
    // Each thread in the WG processes multiple vertices/triangles
    for (uint i = tid; i < vtxCount; i += kTaskShaderWG) {
        uint ind = primitive.Load<uint32_t>(vtxBase + i * sizeof(uint32_t));
        FVertex vtx = primitive.Load<FVertex>(mesh.vtxOffset + ind * sizeof(FVertex));
        vtx.position = inst.rotation.rotate(vtx.position);
        vtx.position = vtx.position * inst.scale + inst.transform;
        V2F v2f;
        v2f.pos = mul(mul(globalParams.proj, globalParams.view), float4(vtx.position, 1.0));
        v2f.meshlet = meshletID;
        verts[i] = v2f;
    }
    for (uint i = tid; i < triCount; i += kTaskShaderWG) {
        MIndex tri = primitive.Load<MIndex>(triBase + i * sizeof(MIndex));
        triangles[i] = uint3(tri.v0, tri.v1, tri.v2);
    }
}