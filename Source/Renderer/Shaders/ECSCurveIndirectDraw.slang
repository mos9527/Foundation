import IGPUScene;
#include "ICommon.slang"

public static const uint32_t kGSInstanceTypeCurve = 1u;

uniform UBO globalParams;
ByteAddressBuffer instances;
ByteAddressBuffer primitive;
ByteAddressBuffer dynamicPrimitives;
RWStructuredBuffer<DrawIndirectCommand> outDrawCmds;
RWStructuredBuffer<uint> outDrawInstanceIDs;
RWStructuredBuffer<Atomic<uint>> outDrawCount;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint tid: SV_DispatchThreadID)
{
    if (tid >= globalParams.instances.count)
        return;
    uint iid = tid + globalParams.instances.offset;
    GSInstance inst = GSLoad<GSInstance>(instances, iid);
    if (GSInstanceType(inst) != kGSInstanceTypeCurve)
        return;
    GSCurveSet curve = GSLoadPrimitive<GSCurveSet>(primitive, dynamicPrimitives, GSInstanceIsDynamic(inst),
                                                 inst.resourceOffset);
    uint draw = outDrawCount[0].add(1);
    DrawIndirectCommand cmd;
    cmd.vertexCount = curve.indices.count;
    cmd.instanceCount = 1;
    cmd.firstVertex = 0;
    cmd.firstInstance = 0;
    outDrawCmds[draw] = cmd;
    outDrawInstanceIDs[draw] = iid;
}
