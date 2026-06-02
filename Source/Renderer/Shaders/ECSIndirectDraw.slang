// Builds the GPU-driven indirect draw list for CPU-updateable dynamic geometry: one
// VkDrawIndexedIndirectCommand per dynamic instance, consumed by EVSIndirectDraw +
// the shared EPSGBuffer fragment through DrawIndexedIndirectCount. Dynamic geo carries no
// DAG/meshlets, so it never enters the meshlet pipeline (ECSCullInstances skips it); this is
// its dedicated draw path. The per-draw absolute instance index is published in a parallel
// buffer indexed by the draw (SV_DrawIndex) - firstInstance is unreliable across HLSL/Vulkan.
#include "ICommon.slang"

uniform UBO globalParams;
StructuredBuffer<GSInstance> instances;
ByteAddressBuffer dynamicPrimitives; // current frame slot; header carries slot-correct offsets
RWStructuredBuffer<DrawIndexedIndirectCommand> outDrawCmds;
RWStructuredBuffer<uint> outDrawInstanceIDs; // [drawIndex] -> absolute scene instance index
RWStructuredBuffer<Atomic<uint>> outDrawCount;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint tid: SV_DispatchThreadID)
{
    if (tid >= globalParams.numInstances)
        return;
    uint iid = tid + globalParams.firstInstance;
    GSInstance inst = instances[iid];
    if (GSInstanceType(inst) != 0u || !GSInstanceIsDynamic(inst))
        return; // only dynamic meshes are drawn here
    // resourceOffset is the current frame slot's absolute header base (resolved in EndScene).
    GSMesh mesh = dynamicPrimitives.Load<GSMesh>(inst.resourceOffset);
    uint draw = outDrawCount[0].add(1);
    DrawIndexedIndirectCommand cmd;
    cmd.indexCount = mesh.idxCount;
    cmd.instanceCount = 1;
    cmd.firstIndex = mesh.idxOffset / 4u; // dynamic ring bound as a UINT32 index buffer
    cmd.vertexOffset = 0;
    cmd.firstInstance = 0;
    outDrawCmds[draw] = cmd;
    outDrawInstanceIDs[draw] = iid; // read by the VS via SV_DrawIndex
}
