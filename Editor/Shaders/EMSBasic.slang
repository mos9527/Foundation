#include "ICommon.slang"

uniform UBO globalParams;
StructuredBuffer<GSInstance> instances;
ByteAddressBuffer primitive;


// Each WG of mesh shader processes one meshlet
// Each thread in the WG processes zero, or more vertices/triangles
[outputtopology("triangle")]
[numthreads(kMeshWorkGroupSize, 1, 1)]
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
    GSMesh mesh = primitive.Load<GSMesh>(inst.meshOffset);
    FMeshlet meshlet = primitive.Load<FMeshlet>(mesh.meshletOffset + meshletID * sizeof(FMeshlet));
    uint32_t vtxBase = mesh.meshletVtxOffset + meshlet.vtxOffset * sizeof(uint32_t);
    uint32_t vtxCount = meshlet.vtxCount;
    uint32_t triBase = mesh.meshletTriOffset + meshlet.triOffset * sizeof(uint8_t);
    uint32_t triCount = meshlet.triCount;
    SetMeshOutputCounts(vtxCount, triCount);
    // Each thread in the WG processes multiple vertices/triangles
    for (uint i = tid; i < vtxCount; i += kMeshWorkGroupSize) {
        uint ind = primitive.Load<uint32_t>(vtxBase + i * sizeof(uint32_t));
        FQVertex fq = primitive.Load<FQVertex>(mesh.vtxOffset + ind * sizeof(FQVertex));
        float3 position, normal, tangent;
        float2 uv;
        float bitangentSign;
        FQDecode(fq, position, normal, tangent, bitangentSign, uv);
        position = inst.rotation.rotate(position);
        position = position * inst.scale + inst.transform;
        normal = inst.rotation.rotate(normal);
        tangent = inst.rotation.rotate(tangent);
        V2F v2f;
        v2f.pos = mul(mul(globalParams.proj, globalParams.view), float4(position, 1.0));
        v2f.normal = normal;
        v2f.tangent = tangent;
        v2f.bitangent = cross(normal, tangent) * bitangentSign;
        v2f.uv = uv;
        v2f.meshlet = mesh.meshletGlobalIndex + meshletID;
        v2f.materialIndex = inst.materialIndex;
        verts[i] = v2f;
    }
    for (uint i = tid; i < triCount; i += kMeshWorkGroupSize) {
        MIndex tri = primitive.Load<MIndex>(triBase + i * sizeof(MIndex));
        triangles[i] = uint3(tri.v0, tri.v1, tri.v2);
    }
}