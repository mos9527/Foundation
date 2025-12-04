#include "IMeshletCommon.slang"

RWStructuredBuffer<Atomic<uint>> counter;
RWStructuredBuffer<MeshletTaskDispatch> dispatch;


[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint tid: SV_DispatchThreadID) {
    // As per VkPhysicalDeviceMeshShaderPropertiesEXT::maxTaskWorkGroupCount
    // min requirements are (65535,65535,65535)
    // Local size for task shader is [kMeshWorkGroupSize,1 ,1]
    // We'll do [ceil(N/kMeshWorkGroupSize),min(N,kMeshWorkGroupSize),1] dispatches for N tasks - task shaders can read the
    // actual task count from the prepared count buffer
    MeshletTaskDispatch command;
    uint N = counter[0].load();    
    command.groupCountX = (N + kMeshWorkGroupSize - 1) / kMeshWorkGroupSize;
    command.groupCountY = kMeshWorkGroupSize;
    command.groupCountZ = 1;
    dispatch[0] = command;
}