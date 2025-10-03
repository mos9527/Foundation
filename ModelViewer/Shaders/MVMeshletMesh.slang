#include "MVMeshlet.h"
struct MicroIndices {
    uint8_t v0;
    uint8_t v1;
    uint8_t v2;
}
// Each WG of mesh shader processes one meshlet
[outputtopology("triangle")]
[numthreads(kMaxMeshShaderWork, 1, 1)]
void meshletMesh(
    in payload MeshletPayload payload,
    in uint meshletID: SV_GroupID,
    in uint groupIndex: SV_GroupIndex,
    OutputVertices<VertexOutput, 64> verts,
    OutputIndices<uint3, 96> triangles
) {    
    if (meshletID >= payload.meshletCount)
        return; // Out of range

    uint offset = payload.meshletRawOffsets[meshletID];
    MeshMeshlet meshlet = sceneConst.Load<MeshMeshlet>(offset);
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);
    uint meshletVerticesRawOffset = meshlet.vertexOffset * sizeof(uint) + payload.meshletVerticesRawOffset;
    // Vertices
    for (uint i = groupIndex; i < meshlet.vertexCount; i += kMaxMeshShaderWork) {
        uint vertex = sceneConst.Load<uint>(meshletVerticesRawOffset + i * sizeof(uint));
        MeshVertexCompact input = sceneConst.Load<MeshVertexCompact>(payload.vertexRawOffset + vertex * sizeof(MeshVertexCompact));
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
        output.meshlet = meshletID;
        verts[i] = output;
    }
    // Indices
    uint triangleRawOffset = meshlet.triangleOffset + payload.meshletTrianglesRawOffset;
    for (uint i = groupIndex; i < meshlet.triangleCount; i += kMaxMeshShaderWork) {
        // Requires 8bit storage
        MicroIndices mi = sceneConst.Load<MicroIndices>(triangleRawOffset + i * sizeof(MicroIndices));
        triangles[i] = uint3(mi.v0, mi.v1, mi.v2);
    }
}
