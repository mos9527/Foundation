#include "Common.h"

StructuredBuffer<PrimitiveMetadata> scPrimitive;
StructuredBuffer<InstanceMetadata> scInstance;
RWStructuredBuffer<Atomic<uint>> counter;
RWStructuredBuffer<MeshDrawIndirectCmd> commands;

[shader("compute")]
[numthreads(32, 1, 1)]
void indirectCullEarly(uint instanceID : SV_DispatchThreadID){
    InstanceMetadata instance = scInstance.Load(instanceID);
    if (instance.primitiveID == 0)
        return;
    PrimitiveMetadata primitive = scPrimitive.Load(instance.primitiveID - 1);
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
