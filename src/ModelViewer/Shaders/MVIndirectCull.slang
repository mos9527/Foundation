#include "Common.h"
[[vk::binding(0, 0)]]
StructuredBuffer<PrimitiveMetadata> scPrimitive;
[[vk::binding(1, 0)]]
StructuredBuffer<InstanceMetadata> scInstance;
[[vk::binding(2, 0)]]
RWStructuredBuffer<Atomic<uint>> counter;
[[vk::binding(3, 0)]]
RWStructuredBuffer<MeshDrawIndirectCmd> commands;

[shader("compute")]
[numthreads(32, 1, 1)]
void indirectCullEarly(uint instanceID : SV_DispatchThreadID){
    InstanceMetadata instance = scInstance.Load(instanceID);
    if (!instance.enabled) return;
    PrimitiveMetadata primitive = scPrimitive.Load(instance.primitiveID);
    MeshDrawIndirectCmd cmd;
    cmd.firstInstance = 0;
    cmd.instanceCount = 1;
    cmd.firstIndex = primitive.indexOffset;
    cmd.indexCount = primitive.indexCount;
    cmd.vertexOffset = primitive.vertexOffset;
    uint commandIndex = counter[0].increment();
    commands[commandIndex] = cmd;
}
