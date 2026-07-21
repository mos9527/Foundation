#include "MeshShaderCommon.slang"
struct UBO
{
    float4x4 view;
    float4x4 proj;
    float zNear;
    float threshold;
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

float projErrorSimple(FLODGroup lodGroup) {
    float3 center = mul(globalParams.view, float4(lodGroup.center, 1.0)).xyz;
    float r = lodGroup.radius;
    float d = max(length(center) - r, globalParams.zNear);
    return lodGroup.error / d * abs(globalParams.proj[1][1]) * 0.5f;
}

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
    // Each WG of mesh shader processes *exactly* one meshlet per dispatch
    FMeshlet meshlet = mesh.Load<FMeshlet>(globalParams.mesh.meshlets.offset + gid * sizeof(FMeshlet));
    uint32_t vtxBase = globalParams.mesh.meshletVtxOffset + meshlet.vtxOffset * sizeof(uint32_t);
    uint32_t vtxCount = meshlet.vtxCount;
    uint32_t triBase = globalParams.mesh.meshletTriOffset + meshlet.triOffset * sizeof(uint8_t);
    uint32_t triCount = meshlet.triCount;
    // View-dependent culling
    FLODGroup selfGroup = mesh.Load<FLODGroup>(globalParams.mesh.groups.offset + meshlet.group * sizeof(FLODGroup));
    float selfError = projErrorSimple(selfGroup);
    // Remember - we take the upper_bound of the acceptable clusters
    // where it's the *first* LOD that exceeds the error threshold
    bool culled = selfError <= globalParams.threshold;
    if (meshlet.refined != ~0u){
        FLODGroup refGroup =
            mesh.Load<FLODGroup>(globalParams.mesh.groups.offset + meshlet.refined * sizeof(FLODGroup));
        float refError = projErrorSimple(refGroup);
        // The refined ones exceeds it too - impossible unless it's closer to
        // the actual upper bound. So cull ourselves.
        culled |= refError > globalParams.threshold;
    }
    if (culled) {
        SetMeshOutputCounts(0, 0);
        return;
    } else {
        SetMeshOutputCounts(vtxCount, triCount);
        // Each thread in the WG processes multiple vertices/triangles
        for (uint i = tid; i < vtxCount; i += kWGSize) {
            uint ind = mesh.Load<uint32_t>(vtxBase + i * sizeof(uint32_t));
            FVertex vtx = mesh.Load<FVertex>(globalParams.mesh.vertices.offset + ind * sizeof(FVertex));
            V2F v2f;
            v2f.pos = mul(mul(globalParams.proj, globalParams.view), float4(vtx.position, 1.0));
            v2f.meshlet = gid;
            verts[i] = v2f;
        }
        for (uint i = tid; i < triCount; i += kWGSize) {
            MIndex tri = mesh.Load<MIndex>(triBase + i * sizeof(MIndex));
            triangles[i] = uint3(tri.v0, tri.v1, tri.v2);
        }
    }
}