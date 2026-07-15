// Motion-vector VS for TRS-animated DOTS curve instances.
#include "ICommon.slang"

uniform UBO globalParams;
[[vk_binding(1, 0)]] StructuredBuffer<GSInstance> instances;
[[vk_binding(2, 0)]] ByteAddressBuffer primitive;
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
    GSCurveSet curve = primitive.Load<GSCurveSet>(inst.resourceOffset);
    GSCurveDOTSVertex vtx = primitive.Load<GSCurveDOTSVertex>(curve.vtxOffset + vertexID * sizeof(GSCurveDOTSVertex));

    float3 currPos = inst.rotation.rotate(vtx.position) * inst.scale + inst.transform;
    float3 prevPos = inst.prevRotation.rotate(vtx.position) * inst.prevScale + inst.prevTransform;

    float4x4 viewProj = mul(globalParams.proj, globalParams.view);
    float4 currClip = mul(viewProj, float4(currPos, 1.0));
    float4 prevClip = mul(globalParams.previousViewProj, float4(prevPos, 1.0));
    MotionV2F v2f;
    v2f.pos = currClip;
    v2f.currClip = currClip;
    v2f.prevClip = prevClip;
    return v2f;
}
