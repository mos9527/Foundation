// vvv Copy-pasted from C++ sources vvv
static const size_t kNumMeshDiscreteLODs = 5;
struct FVertex {
    float3 position;
    float3 normal;
    float2 uv;
};
struct FMeshlet // same layout as meshopt_Meshlet
{
    /* offsets within meshletVtx and meshletTri arrays with meshlet data */
    uint32_t vtxOffset;  // in vertices
    uint32_t triOffset; // in indices (3*triangles)
    /* number of vertices and triangles used in the meshlet; data is stored in consecutive range defined by offset and count */
    uint32_t vtxCount;
    uint32_t triCount;
    /* bounds */
    float4 centerRadius; // (x,y,z,r)
    float4 coneAxisAngle; // (x,y,z,cos(half solid angle))
    float3 coneApex; // (x,y,z)
};
struct GSMeshLOD
{
    uint32_t indOffset;
    uint32_t indCount;
    uint32_t meshletOffset;
    uint32_t meshletCount;
    uint32_t meshletVtxOffset;
    uint32_t meshletTriOffset;
};
struct GSMesh
{
    uint32_t vtxOffset;
    uint32_t vtxCount;
    uint32_t lodCount;
    GSMeshLOD lod[kNumMeshDiscreteLODs];
};
struct UBO
{
    float4x4 mvp;
    GSMesh mesh;
};
// ^^^
uniform UBO globalParams;
ByteAddressBuffer mesh;

struct MIndex {
    uint8_t v0;
    uint8_t v1;
    uint8_t v2;
}
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
    OutputVertices<V2F, 64> verts,
    OutputIndices<uint3, 96> triangles,
) {    
    GSMeshLOD lod = globalParams.mesh.lod[0];
    // Each WG of mesh shader processes *exactly* one meshlet per dispatch
    FMeshlet meshlet = mesh.Load<FMeshlet>(lod.meshletOffset + gid * sizeof(FMeshlet));
    uint32_t vtxBase = lod.meshletVtxOffset + meshlet.vtxOffset * sizeof(uint32_t);
    uint32_t vtxCount = meshlet.vtxCount;
    uint32_t triBase = lod.meshletTriOffset + meshlet.triOffset * sizeof(uint8_t);
    uint32_t triCount = meshlet.triCount;
    printf("Meshlet %d: vtxCount=%d, triCount=%d\n", gid, vtxCount, triCount);
    SetMeshOutputCounts(0, 0);
    return;
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