#include "MVMeshlet.h"
import "Math/Quaternion";
import "Math/Quantization";
struct MicroIndices {
    uint8_t v0;
    uint8_t v1;
    uint8_t v2;
}
groupshared float3 sharedScreenSpaceVerts[kMaxMeshShaderWork]; // Max vertices per meshlet
// Each WG of mesh shader processes one meshlet
// Each thread in the WG processes zero, or more vertices/triangles
[outputtopology("triangle")]
[numthreads(kMaxMeshShaderWork, 1, 1)]
void meshletMesh(
    in payload MeshletPayload payload,
    in uint meshletID: SV_GroupID,
    in uint groupIndex: SV_GroupIndex,
    OutputVertices<VertexOutput, 64> verts,
    OutputIndices<uint3, 96> triangles,
    OutputPrimitives<PrimitiveOutput, 96> prims,
) {    
    if (meshletID >= payload.meshletCount)
        return; // Out of range. Kill the entire WG - barriers should still be fine
    SceneParams scene = sceneShared.Load<SceneParams>(sceneParamsOffset);
    uint offset = payload.meshletRawOffsets[meshletID];
    MeshMeshlet meshlet = sceneConst.Load<MeshMeshlet>(offset);
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);
    uint meshletVerticesRawOffset = meshlet.vertexOffset * sizeof(uint) + payload.meshletVerticesRawOffset;
    // Vertices
    for (uint i = groupIndex; i < meshlet.vertexCount /* Shouldn't process more than one vert as per max verts count */; i += kMaxMeshShaderWork) {
        uint vertex = sceneConst.Load<uint>(meshletVerticesRawOffset + i * sizeof(uint));
        // Requires 16bit storage
        MeshVertexCompact input = sceneConst.Load<MeshVertexCompact>(payload.vertexRawOffset + vertex * sizeof(MeshVertexCompact));
        VertexOutput output;
        // Unpack our data
        float3 N; float4 T_BSign;
        unpackTBN(input.np, input.tp, N, T_BSign);
        // Apply transforms from instance data
        SceneInstanceData instance = sceneInstance.Load<SceneInstanceData>(payload.instanceID * sizeof(SceneInstanceData));
        Quaternion q = Quaternion(instance.q);
        float3 pos = float3(input.px, input.py, input.pz);
        pos = q.rotate(pos);
        pos = pos + instance.t;
        // Output
        output.pos = mul(scene.camera.viewProj, float4(pos, 1.0));
        output.normal = q.rotate(N);
        output.tangent = N; // q.rotate(T_BSign.xyz);
        output.bitangent = cross(output.normal, output.tangent) * T_BSign.w;
        output.uv = float2(input.u, input.v);
        output.meshlet = meshletID;
        verts[i] = output;
        // For culling
        sharedScreenSpaceVerts[i] = output.pos.xyz / output.pos.w;
    }
    GroupMemoryBarrierWithGroupSync();
    // Indices
    uint triangleRawOffset = meshlet.triangleOffset + payload.meshletTrianglesRawOffset;
    for (uint i = groupIndex; i < meshlet.triangleCount /* Could process more than one tri (64<96) for each thread */; i += kMaxMeshShaderWork) {
        // Requires 8bit storage
        PrimitiveOutput prim;
        prim.id = i;
        prim.cull = false; // Default to not culled
        MicroIndices mi = sceneConst.Load<MicroIndices>(triangleRawOffset + i * sizeof(MicroIndices));
        float3 a = sharedScreenSpaceVerts[mi.v0], b = sharedScreenSpaceVerts[mi.v1], c = sharedScreenSpaceVerts[mi.v2];
        float3 ab = b - a, ac = c - a;
        float prod = ab.x * ac.y - ab.y * ac.x;
        // Cull backface
        if (prod > 0) // Clockwise -> backfacing
            prim.cull = true;        
        triangles[i] = uint3(mi.v0, mi.v1, mi.v2);
        prims[i] = prim;
    }
}
