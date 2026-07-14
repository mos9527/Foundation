// Vertex shader for skinned/morph motion-vector MDI draws.
#include "ICommon.slang"

uniform UBO globalParams;
// Contiguous set-0 bindings (unlike EVSIndirectDraw, which leaves 3..5 for EPSGBuffer).
[[vk_binding(1, 0)]] StructuredBuffer<GSInstance> instances;
[[vk_binding(2, 0)]] ByteAddressBuffer dynamicPrimitives;
[[vk_binding(3, 0)]] StructuredBuffer<uint> drawInstanceIDs;

public struct MotionV2F
{
    public float4 pos : SV_POSITION;
    public float4 currClip : CURRCLIP;
    public float4 prevClip : PREVCLIP;
};

[shader("vertex")]
MotionV2F main(uint vertexID: SV_VertexID, uint drawID: SV_DrawIndex)
{
    uint instanceID = drawInstanceIDs[drawID];
    GSInstance inst = instances[instanceID];
    GSMesh currMesh = dynamicPrimitives.Load<GSMesh>(inst.resourceOffset);
    GSMesh prevMesh = dynamicPrimitives.Load<GSMesh>(inst.prevResourceOffset);
    FQVertex currFQ = dynamicPrimitives.Load<FQVertex>(currMesh.vtxOffset + vertexID * sizeof(FQVertex));
    FQVertex prevFQ = dynamicPrimitives.Load<FQVertex>(prevMesh.vtxOffset + vertexID * sizeof(FQVertex));

    float3 currPos, prevPos, normal, tangent;
    float bitangentSign;
    float2 uv;
    FQDecode(currFQ, currPos, normal, tangent, bitangentSign, uv);
    FQDecode(prevFQ, prevPos, normal, tangent, bitangentSign, uv);

    currPos = inst.rotation.rotate(currPos) * inst.scale + inst.transform;
    prevPos = inst.prevRotation.rotate(prevPos) * inst.prevScale + inst.prevTransform;

    float4x4 viewProj = mul(globalParams.proj, globalParams.view);
    float4 currClip = mul(viewProj, float4(currPos, 1.0));
    float4 prevClip = mul(globalParams.previousViewProj, float4(prevPos, 1.0));
    MotionV2F v2f;
    v2f.pos = currClip;
    v2f.currClip = currClip;
    v2f.prevClip = prevClip;
    return v2f;
}
