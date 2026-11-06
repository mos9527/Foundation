import IGPUScene;
#include "ICommon.slang"

uniform UBO globalParams;
ByteAddressBuffer instances;
ByteAddressBuffer dynamicPrimitives; // stable page table; headers carry virtual byte offsets
RWStructuredBuffer<DrawIndirectCommand> outDrawCmds;
RWStructuredBuffer<uint> outDrawInstanceIDs; // [drawIndex] -> absolute scene instance index
RWStructuredBuffer<Atomic<uint>> outDrawCount;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint tid: SV_DispatchThreadID)
{
    if (tid >= globalParams.instances.count)
        return;
    uint iid = tid + globalParams.instances.offset;
    GSInstance inst = GSLoad<GSInstance>(instances, iid);
    if (GSInstanceType(inst) != 0u || !GSInstanceIsDynamic(inst))
        return; // only dynamic meshes are drawn here
    // resourceOffset is the stable virtual header offset (resolved in EndScene).
    GSMesh mesh = GSLoadPrimitive<GSMesh>(dynamicPrimitives, inst.resourceOffset);
    uint draw = outDrawCount[0].add(1);
    DrawIndirectCommand cmd;
    cmd.vertexCount = mesh.indices.count;
    cmd.instanceCount = 1;
    cmd.firstVertex = 0;
    cmd.firstInstance = 0;
    outDrawCmds[draw] = cmd;
    outDrawInstanceIDs[draw] = iid; // read by the VS via SV_DrawIndex
}
