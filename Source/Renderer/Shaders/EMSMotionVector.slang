// Mesh shader for static motion-vector contributors.
#include "ICommon.slang"

uniform UBO globalParams;

// Contiguous set-0 bindings (unlike EMSBasic, which leaves 3..5 for EPSGBuffer).
[[vk_binding(1, 0)]] StructuredBuffer<GSInstance> instances;
[[vk_binding(2, 0)]] ByteAddressBuffer primitive;
[[vk_binding(3, 0)]] StructuredBuffer<uint> inMeshletCounter;
[[vk_binding(4, 0)]] StructuredBuffer<uint2> inMeshletIndices; // (x: Meshlet ID, y: Instance ID)

public struct MotionV2F
{
    public float4 pos : SV_POSITION;
    public float4 currClip : CURRCLIP;
    public float4 prevClip : PREVCLIP;
};

struct MSGroup
{
    GSInstance instance;
    GSMesh mesh;
    uint32_t meshletID;
    FMeshlet meshlet;
};
groupshared MSGroup group;

[outputtopology("triangle")]
[numthreads(kMeshWorkGroupSize, 1, 1)]
void main(uint2 gid: SV_GroupID, in uint tid: SV_GroupIndex, OutputVertices<MotionV2F, 96> verts,
          OutputIndices<uint3, 96> triangles)
{
    uint drawID = dot(gid, uint2(kMeshIndirectPerDrawMeshlets, 1));
    if (drawID >= inMeshletCounter[0])
        return;
    if (tid == 0)
    {
        uint2 indices = inMeshletIndices[drawID];
        group.meshletID = indices.x;
        group.instance = instances[indices.y];
        group.mesh = primitive.Load<GSMesh>(group.instance.resourceOffset);
        group.meshlet = primitive.Load<FMeshlet>(group.mesh.meshletOffset + group.meshletID * sizeof(FMeshlet));
    }
    GroupMemoryBarrierWithGroupSync();
    uint32_t vtxBase = group.mesh.meshletVtxOffset + group.meshlet.vtxOffset * sizeof(uint32_t);
    uint32_t vtxCount = group.meshlet.vtxCount;
    uint32_t triBase = group.mesh.meshletTriOffset + group.meshlet.triOffset * sizeof(uint8_t);
    uint32_t triCount = group.meshlet.triCount;
    SetMeshOutputCounts(vtxCount, triCount);
    float4x4 viewProj = mul(globalParams.proj, globalParams.view);
    for (uint i = tid; i < vtxCount; i += kMeshWorkGroupSize)
    {
        uint ind = primitive.Load<uint32_t>(vtxBase + i * sizeof(uint32_t));
        FQVertex fq = primitive.Load<FQVertex>(group.mesh.vtxOffset + ind * sizeof(FQVertex));
        float3 position, normal, tangent;
        float2 uv;
        float bitangentSign;
        FQDecode(fq, position, normal, tangent, bitangentSign, uv);
        float3 currWS = group.instance.rotation.rotate(position) * group.instance.scale + group.instance.transform;
        float3 prevWS =
            group.instance.prevRotation.rotate(position) * group.instance.prevScale + group.instance.prevTransform;
        float4 currClip = mul(viewProj, float4(currWS, 1.0));
        float4 prevClip = mul(globalParams.previousViewProj, float4(prevWS, 1.0));
        MotionV2F v2f;
        v2f.pos = currClip;
        v2f.currClip = currClip;
        v2f.prevClip = prevClip;
        verts[i] = v2f;
    }
    for (uint i = tid; i < triCount; i += kMeshWorkGroupSize)
    {
        MIndex tri = primitive.Load<MIndex>(triBase + i * sizeof(MIndex));
        triangles[i] = uint3(tri.v0, tri.v1, tri.v2);
    }
}
