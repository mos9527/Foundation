// TODO: What a mess.
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

    position = inst.rotation.rotate(position * inst.scale) + inst.transform;
    normal = normalize(inst.rotation.rotate(normal / inst.scale));
    tangent = normalize(inst.rotation.rotate(tangent * inst.scale));
    bitangentSign *= sign(inst.scale.x) * sign(inst.scale.y) * sign(inst.scale.z);

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
