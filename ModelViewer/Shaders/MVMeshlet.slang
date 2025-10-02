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
    Atomic<uint32_t> meshletCount;
    uint32_t instanceID;
    uint32_t meshletVerticesRawOffset;
    uint32_t meshletTrianglesRawOffset;
};
groupshared MeshletPayload payload;

[shader("amplification")]
[numthreads(kMaxTaskShaderWork, 1, 1)]
void meshletTask(uint meshlet: SV_GroupThreadID, uint2 groupID: SV_GroupID, uint groupIndex : SV_GroupIndex) {    
    uint count = counter[0];
    if (groupIndex >= count)
        return; // Out of range
    // Initialize shared memory
    payload.meshletCount = 0;
    payload.instanceID = tasks[groupIndex].instanceID;
    payload.meshletVerticesRawOffset = tasks[groupIndex].meshletVerticesRawOffset;
    payload.meshletTrianglesRawOffset = tasks[groupIndex].meshletTrianglesRawOffset;
    GroupMemoryBarrierWithGroupSync();
    MeshletTaskParams params = tasks[groupIndex];
    if (meshlet > params.meshletCount)
        return; // Out of range
    // TODO: Culling
    uint payloadIndex = payload.meshletCount.add(1);
    payload.meshletRawOffsets[payloadIndex] = tasks[groupIndex].meshletRawOffset + meshlet * sizeof(MeshMeshlet);
    GroupMemoryBarrierWithGroupSync();
    // DispatchMesh works in groups
    DispatchMesh(params.meshletCount, 1, 1, payload);
}
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
[outputtopology("triangle")]
[numthreads(kMaxTaskShaderWork, 1, 1)]
void meshletMesh(
    uint meshletIndex: SV_GroupID,
    OutputVertices<VertexOutput, 64> verts,
    OutputIndices<uint3, 32> triangles
) {
    if (meshletIndex > payload.meshletCount.load())
        return;
    uint offset = payload.meshletRawOffsets[meshletIndex];
    MeshMeshlet meshlet = sceneConst.Load<MeshMeshlet>(offset);
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);
    uint vertexRawOffset = meshlet.vertexOffset * sizeof(MeshVertexCompact) + payload.meshletVerticesRawOffset;
    uint triangleRawOffset = meshlet.triangleOffset * sizeof(uint) + payload.meshletTrianglesRawOffset;
    // Vertices
    for (uint i = 0; i < meshlet.vertexCount; i++) {
        MeshVertexCompact input = sceneConst.Load<MeshVertexCompact>(vertexRawOffset + i * sizeof(MeshVertexCompact));
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
    for (uint i = 0; i < meshlet.triangleCount; i++) {
        // XXX: Would alignment be an issue?
        uint3 tri = sceneConst.Load<uint3>(triangleRawOffset + i * sizeof(uint3));
        triangles[i] = tri;
    }
}

[shader("fragment")]
float4 meshletFrag(VertexOutput vtx) : SV_Target {
    return float4(float3(1) * clamp(dot(vtx.normal, float3(0, 0, 1)), 0, 1), 1.0);
}
