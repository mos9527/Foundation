// Vertex shader for the CPU-updateable dynamic geometry indirect draw. Dynamic geo skips the
// meshlet/DAG pipeline and is drawn with a plain indexed draw (DrawIndexedIndirectCount): the
// dynamic primitive ring is bound as a UINT32 index buffer, so SV_VertexID is the mesh-local
// vertex index, and SV_InstanceID carries the absolute scene instance index (set via
// firstInstance in the indirect command). Output matches EMSBasic's V2F and uses bindings 0..2
// so the shared EPSGBuffer fragment (bindings 3..5) produces an identical gbuffer.
#include "ICommon.slang"

uniform UBO globalParams;
[[vk_binding(1, 0)]] StructuredBuffer<GSInstance> instances;
[[vk_binding(2, 0)]] ByteAddressBuffer dynamicPrimitives; // current frame slot
// Binding 6 (after EPSGBuffer's 3..5): absolute instance index per indirect draw.
[[vk_binding(6, 0)]] StructuredBuffer<uint> drawInstanceIDs;

[shader("vertex")]
V2F main(uint vertexID: SV_VertexID, uint drawID: SV_DrawIndex)
{
    // SV_InstanceID is unreliable here (HLSL excludes firstInstance); index by the draw instead.
    uint instanceID = drawInstanceIDs[drawID];
    GSInstance inst = instances[instanceID];
    GSMesh mesh = dynamicPrimitives.Load<GSMesh>(inst.resourceOffset);
    FQVertex fq = dynamicPrimitives.Load<FQVertex>(mesh.vertices.offset + vertexID * sizeof(FQVertex));

    float3 position, normal, tangent;
    float bitangentSign;
    float2 uv;
    FQDecode(fq, position, normal, tangent, bitangentSign, uv);

    position = inst.rotation.rotate(position);
    position = position * inst.scale + inst.transform;
    normal = inst.rotation.rotate(normal);
    tangent = inst.rotation.rotate(tangent);

    V2F v2f;
    v2f.pos = mul(mul(globalParams.proj, globalParams.view), float4(position, 1.0));
    v2f.normal = normal;
    v2f.tangent = tangent;
    v2f.bitangent = cross(normal, tangent) * bitangentSign;
    v2f.bitangentSign = bitangentSign;
    v2f.uv = uv;
    v2f.meshlet = 0u; // dynamic geo has no meshlets
    v2f.materialIndex = inst.materialIndex;
    v2f.instanceID = instanceID;
    return v2f;
}
