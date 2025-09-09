#include "Common.h"

StructuredBuffer<PrimitiveMetadata> scPrimitive;
StructuredBuffer<InstanceMetadata> scInstance;

RWStructuredBuffer<Atomic<uint>> counter;
RWStructuredBuffer<MeshDrawIndirectCmd> commands;

[shader("compute")]
[numthreads(32, 1, 1)]
void indirectCullEarly(uint instanceID : SV_DispatchThreadID){
    InstanceMetadata instance = scInstance.Load(instanceID);
    if (!instance.enabled) return;
    PrimitiveMetadata primitive = scPrimitive.Load(instance.primitiveID);
    uint commandIndex = counter[0].load();
    MeshDrawIndirectCmd cmd;
    cmd.firstInstance = 0;
    cmd.instanceCount = 1;
    cmd.firstIndex = primitive.indexOffset;
    cmd.indexCount = primitive.indexCount;
    cmd.vertexOffset = primitive.vertexOffset;
    counter[0].increment();
}
