#include "Common.h"

ByteAddressBuffer scPrimitive;
StructuredBuffer<InstanceData> scInstance;
RWStructuredBuffer<Atomic<uint>> counter;
RWStructuredBuffer<MeshDrawIndirectCmd> commands;

[shader("compute")]
[numthreads(32, 1, 1)]
void indirectCullEarly(uint instanceID : SV_DispatchThreadID){
    InstanceData instance = scInstance.Load(instanceID);
    if (instance.primitiveOffsetPP == 0)
        return;
    PrimitiveData primitive = scPrimitive.Load<PrimitiveData>(instance.primitiveOffsetPP - 1);
    MeshDrawIndirectCmd cmd;
    cmd.instanceID = instanceID;
    cmd.firstInstance = 0;
    cmd.instanceCount = 1;
    cmd.firstIndex = primitive.indexOffset;
    cmd.indexCount = primitive.indexCount;
    cmd.vertexOffset = primitive.vertexOffset;
    uint commandIndex = counter[0].increment();
    commands[commandIndex] = cmd;
}
