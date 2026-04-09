#include "ICommon.slang"

uniform UBO globalParams;

[[vk_binding(1,0)]] StructuredBuffer<GSInstance> instances;
[[vk_binding(2,0)]] ByteAddressBuffer primitive;
[[vk_binding(3,0)]] StructuredBuffer<uint> inMeshletCounter;
[[vk_binding(4,0)]] StructuredBuffer<uint2> inMeshletIndices; // (x: Meshlet ID, y: Instance ID)

struct MSGroup {
    GSInstance instance;
    GSMesh mesh;
    uint32_t meshletID;
    FMeshlet meshlet;
}
groupshared MSGroup group;
// Each WG of mesh shader processes one meshlet
// Each thread in the WG processes zero, or more vertices/triangles
[outputtopology("triangle")]
[numthreads(kMeshWorkGroupSize, 1, 1)]
void main(
    uint2 gid: SV_GroupID,
    in uint tid: SV_GroupIndex,
    OutputVertices<V2F, 96> verts,
    OutputIndices<uint3, 96> triangles
) {
    uint drawID = dot(gid, uint2(kMeshIndirectPerDrawMeshlets, 1));
    if (drawID >= inMeshletCounter[0]) return;
    if (tid == 0){
        // Linear dispatch, grouped by kMeshIndirectPerDrawMeshlets
        uint2 indices = inMeshletIndices[drawID];
        group.meshletID = indices.x;
        group.instance = instances[indices.y];
        group.mesh = primitive.Load<GSMesh>(group.instance.meshOffset);
        group.meshlet = primitive.Load<FMeshlet>(group.mesh.meshletOffset + group.meshletID * sizeof(FMeshlet));
    }
    GroupMemoryBarrierWithGroupSync();
    uint32_t vtxBase = group.mesh.meshletVtxOffset + group.meshlet.vtxOffset * sizeof(uint32_t);
    uint32_t vtxCount = group.meshlet.vtxCount;
    uint32_t triBase = group.mesh.meshletTriOffset + group.meshlet.triOffset * sizeof(uint8_t);
    uint32_t triCount = group.meshlet.triCount;
    // Each thread in the WG processes multiple vertices/triangles
    SetMeshOutputCounts(vtxCount, triCount);
    for (uint i = tid; i < vtxCount; i += kMeshWorkGroupSize) {
        uint ind = primitive.Load<uint32_t>(vtxBase + i * sizeof(uint32_t));
        FQVertex fq = primitive.Load<FQVertex>(group.mesh.vtxOffset + ind * sizeof(FQVertex));
        float3 position, normal, tangent;
        float2 uv;
        float bitangentSign;
        FQDecode(fq, position, normal, tangent, bitangentSign, uv);
        position = group.instance.rotation.rotate(position);
        position = position * group.instance.scale + group.instance.transform;
        normal = group.instance.rotation.rotate(normal);
        tangent = group.instance.rotation.rotate(tangent);
        V2F v2f;
        v2f.pos = mul(mul(globalParams.proj, globalParams.view), float4(position, 1.0));
        v2f.normal = normal;
        v2f.tangent = tangent;
        v2f.bitangent = cross(normal, tangent) * bitangentSign;
        v2f.bitangentSign = bitangentSign;
        v2f.uv = uv;
        v2f.meshlet = group.mesh.meshletGlobalIndex + group.meshletID;
        v2f.materialIndex = group.instance.materialIndex;
        v2f.instanceID = inMeshletIndices[drawID].y;
        verts[i] = v2f;
    }
    for (uint i = tid; i < triCount; i += kMeshWorkGroupSize) {
        MIndex tri = primitive.Load<MIndex>(triBase + i * sizeof(MIndex));
        triangles[i] = uint3(tri.v0, tri.v1, tri.v2);
    }
}