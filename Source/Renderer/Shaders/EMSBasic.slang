import IGPUScene;
#include "ICommon.slang"

uniform UBO globalParams;

ByteAddressBuffer instances;
ByteAddressBuffer primitive;
StructuredBuffer<uint> inMeshletCounter;
StructuredBuffer<uint2> inMeshletIndices; // (x: Meshlet ID, y: Instance ID)

struct MSGroup {
    GSInstance instance;
    GSMesh mesh;
    uint32_t meshletID;
    uint32_t instanceID;
    FMeshlet meshlet;
    bool valid;
}
groupshared MSGroup group;

bool loadMeshGroup(uint drawID)
{
    uint recordCount, recordStride;
    inMeshletIndices.GetDimensions(recordCount, recordStride);
    if (drawID >= recordCount)
        return false;
    uint2 indices = inMeshletIndices[drawID];
    group.meshletID = indices.x;
    group.instanceID = indices.y;
    if (indices.y < globalParams.instances.offset ||
        indices.y - globalParams.instances.offset >= globalParams.instances.count)
        return false;
    GSAddressInfo info = GSInspectElementAddress<GSInstance>(instances, indices.y);
    if (info.status != 0u || (info.address & uint64_t(3)) != 0)
        return false;
    group.instance = *Ptr<GSInstance, Access.Read, AddressSpace.Device, ScalarDataLayout>(info.address);
    if (group.instance.materialIndex >= globalParams.materials.count ||
        group.instance.materialIndex > ~0u - globalParams.materials.offset)
        return false;
    if (group.instance.resourceOffset == ~0u)
        return false;
    info = GSInspectPrimitiveAddress(primitive, group.instance.resourceOffset);
    if (info.status != 0u || (info.address & uint64_t(3)) != 0)
        return false;
    group.mesh = *Ptr<GSMesh, Access.Read, AddressSpace.Device, ScalarDataLayout>(info.address);
    if (indices.x >= group.mesh.meshlets.count ||
        indices.x > (~0u - group.mesh.meshlets.offset) / sizeof(FMeshlet))
        return false;
    info = GSInspectPrimitiveAddress(primitive, group.mesh.meshlets.offset + indices.x * sizeof(FMeshlet));
    if (info.status != 0u || (info.address & uint64_t(3)) != 0)
        return false;
    group.meshlet = *Ptr<FMeshlet, Access.Read, AddressSpace.Device, ScalarDataLayout>(info.address);
    if (group.meshlet.vtxCount > 96u || group.meshlet.triCount > 96u)
        return false;
    return true;
}
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
    if (drawID >= inMeshletCounter[0])
    {
        SetMeshOutputCounts(0u, 0u);
        return;
    }
    if (tid == 0)
        group.valid = loadMeshGroup(drawID);
    GroupMemoryBarrierWithGroupSync();
    if (!group.valid)
    {
        SetMeshOutputCounts(0u, 0u);
        return; // Workgroup-uniform exit after the barrier; never leave other lanes waiting.
    }
    uint32_t vtxBase = group.mesh.meshletVtxOffset + group.meshlet.vtxOffset * sizeof(uint32_t);
    uint32_t vtxCount = group.meshlet.vtxCount;
    uint32_t triBase = group.mesh.meshletTriOffset + group.meshlet.triOffset * sizeof(uint8_t);
    uint32_t triCount = group.meshlet.triCount;
    // Each thread in the WG processes multiple vertices/triangles
    SetMeshOutputCounts(vtxCount, triCount);
    for (uint i = tid; i < vtxCount; i += kMeshWorkGroupSize) {
        uint ind = GSLoadPrimitive<uint32_t>(primitive, vtxBase + i * sizeof(uint32_t));
        FQVertex fq = GSLoadPrimitive<FQVertex>(primitive, group.mesh.vertices.offset + ind * sizeof(FQVertex));
        float3 position, normal, tangent;
        float2 uv;
        float bitangentSign;
        FQDecode(fq, position, normal, tangent, bitangentSign, uv);
        position = group.instance.rotation.rotate(position * group.instance.scale) + group.instance.transform;
        normal = normalize(group.instance.rotation.rotate(normal / group.instance.scale));
        tangent = normalize(group.instance.rotation.rotate(tangent * group.instance.scale));
        bitangentSign *= sign(group.instance.scale.x) * sign(group.instance.scale.y) * sign(group.instance.scale.z);
        V2F v2f;
        v2f.pos = mul(mul(globalParams.proj, globalParams.view), float4(position, 1.0));
        v2f.normal = normal;
        v2f.tangent = tangent;
        v2f.bitangent = cross(normal, tangent) * bitangentSign;
        v2f.uv = uv;
        v2f.meshlet = group.mesh.meshletGlobalIndex + group.meshletID;
        v2f.materialIndex = group.instance.materialIndex;
        v2f.instanceID = group.instanceID;
        verts[i] = v2f;
    }
    for (uint i = tid; i < triCount; i += kMeshWorkGroupSize) {
        MIndex tri = GSLoadPrimitive<MIndex>(primitive, triBase + i * sizeof(MIndex));
        triangles[i] = uint3(tri.v0, tri.v1, tri.v2);
    }
}