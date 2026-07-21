#include "ICommon.slang"

public static const uint32_t kGSInstanceTypeCurve = 1u;

uniform UBO globalParams;
StructuredBuffer<GSInstance> instances;
ByteAddressBuffer primitive;
RWStructuredBuffer<DrawIndexedIndirectCommand> outDrawCmds;
RWStructuredBuffer<uint> outDrawInstanceIDs;
RWStructuredBuffer<Atomic<uint>> outDrawCount;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint tid: SV_DispatchThreadID)
{
    if (tid >= globalParams.numInstances)
        return;
    uint iid = tid + globalParams.firstInstance;
    GSInstance inst = instances[iid];
    if (GSInstanceType(inst) != kGSInstanceTypeCurve)
        return;
    GSCurveSet curve = primitive.Load<GSCurveSet>(inst.resourceOffset);
    uint draw = outDrawCount[0].add(1);
    DrawIndexedIndirectCommand cmd;
    cmd.indexCount = curve.idxCount;
    cmd.instanceCount = 1;
    cmd.firstIndex = curve.idxOffset / 4u;
    cmd.vertexOffset = 0;
    cmd.firstInstance = 0;
    outDrawCmds[draw] = cmd;
    outDrawInstanceIDs[draw] = iid;
}
