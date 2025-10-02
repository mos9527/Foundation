import "Common/Definitions";
import "Math/Bits";
import "Math/Quantization";
import "Math/Quaternion";

StructuredBuffer<MeshletTaskParams> tasks;
StructuredBuffer<uint> counter;
ByteAddressBuffer sceneConst;
ByteAddressBuffer sceneInstance;

struct DrawPushConstant
{
    float4x4 viewProj;
    float time;
    float3 _pad;
};
[[vk::push_constant]] DrawPushConstant draw;

struct MeshletPayload
{
    uint32_t meshletRawOffsets[kMaxTaskShaderWork];
    uint32_t meshletCount;
    uint32_t instanceID;
    uint32_t meshletVerticesRawOffset;
    uint32_t meshletTrianglesRawOffset;
};
groupshared MeshletPayload gsPayload;
// https://gpuopen.com/learn/mesh_shaders/mesh_shaders-optimization_and_best_practices/
[shader("amplification")]
[numthreads(kMaxTaskShaderWork, 1, 1)]
void meshletTask(uint meshlet: SV_GroupIndex, uint2 groupID: SV_GroupID) {
    uint count = counter[0];
    uint dispatchIndex = dot(groupID, uint2(kMaxTaskShaderWork, 1));
    if (dispatchIndex >= count)
        return; // Out of range - kill the entire WG
    MeshletTaskParams params = tasks[dispatchIndex];
    // Initialize payload    
    gsPayload.instanceID = params.instanceID;
    gsPayload.meshletVerticesRawOffset = params.meshletVerticesRawOffset;
    gsPayload.meshletTrianglesRawOffset = params.meshletTrianglesRawOffset;
    bool visible = meshlet < params.meshletCount;
    // Not used - note to self: do NOT branch on a memory barrier!! i.e. GroupMemoryBarrierWithGroupSync();
    // TODO: Actual Culling
    if (visible) {
        uint gsPayloadIndex = WavePrefixCountBits(visible);
        gsPayload.meshletRawOffsets[gsPayloadIndex] = params.meshletRawOffset + meshlet * sizeof(MeshMeshlet);
    }    
    gsPayload.meshletCount = WaveActiveCountBits(visible);
    // DispatchMesh works in groups - this 'effectively' runs once
    printf("dispatch %d meshlets %d", dispatchIndex, gsPayload.meshletCount);
    DispatchMesh(gsPayload.meshletCount, 1, 1, gsPayload);
}
struct MeshVertexCompact32 {
    uint32_t pxy;
    uint32_t pztp;
    uint32_t np;
    uint32_t uv;
};
struct MeshVertexCompact {
    uint16_t px, py, pz; // quantized fp16
    uint16_t tp;         // tangent [octa 8+8]
    uint32_t np;         // normal packed [snorm octa 15+15, bitangent sign 2]
    uint16_t u, v;       // texcoord fp16
};
struct VertexOutput
{
    float4 pos;
    float3 normal;
    float3 tangent;
    float3 bitangent;
    float2 uv;
};

// Each WG of mesh shader processes one meshlet
[outputtopology("triangle")]
[numthreads(kMaxTaskShaderWork, 1, 1)]
void meshletMesh(
    in payload MeshletPayload payload,
    in uint meshlet: SV_GroupID,
    in uint groupIndex: SV_GroupIndex,
    OutputVertices<VertexOutput, 64> verts,
    OutputIndices<uint3, 32> triangles
) {
    printf("meshlet %d gid %d count %d addr %d", meshlet, groupIndex, payload.meshletCount, payload.meshletVerticesRawOffset);
    if (meshlet >= payload.meshletCount)
        return; // Out of range - shouldn't happen?

    uint offset = payload.meshletRawOffsets[meshlet];
    MeshMeshlet meshlet = sceneConst.Load<MeshMeshlet>(offset);
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);

    uint vertexRawOffset = meshlet.vertexOffset * sizeof(MeshVertexCompact) + payload.meshletVerticesRawOffset;
    uint triangleRawOffset = meshlet.triangleOffset * sizeof(uint) + payload.meshletTrianglesRawOffset;
    // Vertices
    for (uint i = groupIndex; i < meshlet.vertexCount; i += kMaxTaskShaderWork) {
        MeshVertexCompact32 input32 = sceneConst.Load<MeshVertexCompact32>(vertexRawOffset + i * sizeof(MeshVertexCompact32));
        MeshVertexCompact input;
        input.px = input32.pxy & 0xFFFF;
        input.py = (input32.pxy >> 16) & 0xFFFF;
        input.pz = input32.pztp & 0xFFFF;
        input.tp = (input32.pztp >> 16) & 0xFFFF;
        input.np = input32.np;
        input.u = input32.uv & 0xFFFF;
        input.v = (input32.uv >> 16) & 0xFFFF;
        VertexOutput output;
        
        float3 N; float4 T_BSign;
        unpackTBN(input.np, input.tp, N, T_BSign);
        // Apply transforms from instance data
        SceneInstanceData instance = sceneInstance.Load<SceneInstanceData>(payload.instanceID * sizeof(SceneInstanceData));
        Quaternion q = Quaternion(instance.q);
        float3 pos = float3(input.px, input.py, input.pz);
        pos = q.rotate(pos);
        pos = pos + instance.t;
        // Output
        output.pos = mul(draw.viewProj, float4(pos, 1.0));
        output.normal = q.rotate(N);
        output.tangent = q.rotate(T_BSign.xyz);
        output.bitangent = cross(output.normal, output.tangent) * T_BSign.w;
        output.uv = float2(input.u, input.v);
        verts[i] = output;
    }
    // Indices
    for (uint i = groupIndex; i < meshlet.triangleCount; i += kMaxTaskShaderWork) {
        // XXX: Would alignment be an issue?
        uint a, b, c;
        a = sceneConst.Load<uint>(triangleRawOffset + (i * 3 + 0) * sizeof(uint));
        b = sceneConst.Load<uint>(triangleRawOffset + (i * 3 + 1) * sizeof(uint));
        c = sceneConst.Load<uint>(triangleRawOffset + (i * 3 + 2) * sizeof(uint));        
        triangles[i] = uint3(a, b, c);
    }
}

[shader("fragment")]
float4 meshletFrag(VertexOutput vtx) : SV_Target {
    return float4(float3(1) * clamp(dot(vtx.normal, float3(0, 0, 1)), 0, 1), 1.0);
}
