#include "MeshShaderCommon.slang"
struct UBO
{
    float4x4 mvp;
    uint32_t cutDepth;
    GSMesh mesh;
};
// ^^^
uniform UBO globalParams;
ByteAddressBuffer mesh;


struct V2F
{
    float4 pos : SV_POSITION;
    uint32_t meshlet : ID;
};

static const size_t kWGSize = 64;
[outputtopology("triangle")]
[numthreads(kWGSize, 1, 1)]
void meshMain(
    // See graphics reference available at:
    // * https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sv-groupindex
    in uint gid: SV_GroupID,
    in uint tid: SV_GroupIndex,
    OutputVertices<V2F, 96> verts,
    OutputIndices<uint3, 96> triangles,
) {    
    GSMeshLOD lod = globalParams.mesh.lod[0];
    // Each WG of mesh shader processes *exactly* one meshlet per dispatch
    FMeshlet meshlet = mesh.Load<FMeshlet>(lod.meshletOffset + gid * sizeof(FMeshlet));
    uint32_t vtxBase = lod.meshletVtxOffset + meshlet.vtxOffset * sizeof(uint32_t);
    uint32_t vtxCount = meshlet.vtxCount;
    uint32_t triBase = lod.meshletTriOffset + meshlet.triOffset * sizeof(uint8_t);
    uint32_t triCount = meshlet.triCount;
    uint32_t groupBase = globalParams.mesh.groupOffset + meshlet.group * sizeof(FLODGroup);
    FLODGroup lodGroup = mesh.Load<FLODGroup>(groupBase);
    if (lodGroup.depth != globalParams.cutDepth) { // Cull Depth
        SetMeshOutputCounts(0, 0);
        return;
    }
    SetMeshOutputCounts(vtxCount, triCount);
    // Each thread in the WG processes multiple vertices/triangles
    for (uint i = tid; i < vtxCount; i += kWGSize) {
        uint ind = mesh.Load<uint32_t>(vtxBase + i * sizeof(uint32_t));
        FVertex vtx = mesh.Load<FVertex>(globalParams.mesh.vtxOffset + ind * sizeof(FVertex));
        V2F v2f;
        v2f.pos = mul(globalParams.mvp, float4(vtx.position, 1.0));
        v2f.meshlet = gid;
        verts[i] = v2f;
    }
    for (uint i = tid; i < triCount; i += kWGSize) {
        MIndex tri = mesh.Load<MIndex>(triBase + i * sizeof(MIndex));
        triangles[i] = uint3(tri.v0, tri.v1, tri.v2);
    }
}