import "Scene/Structs";
import "Math/Bits";

RWStructuredBuffer<Atomic<uint>> counter;
RWStructuredBuffer<MeshletTaskDispatch> dispatch;

[shader("compute")]
[numthreads(1, 1, 1)]
void taskSubmit() {
    // As per VkPhysicalDeviceMeshShaderPropertiesEXT::maxTaskWorkGroupCount
    // min requirements are (65535,65535,65535)
    // Local size for task shader is [kMaxTaskShaderWork,1 ,1]
    // Note to self: https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-dispatch#remarks
    // We'll do [ceil(N/kMaxTaskShaderWork),min(N,kMaxTaskShaderWork),1] dispatches for N tasks - task shaders can read the
    // actual task count from the prepared count buffer
    MeshletTaskDispatch command;
    uint N = counter[0].load();        
    command.groupCountX = (N + kMaxTaskShaderWork - 1) / kMaxTaskShaderWork;
    command.groupCountY = kMaxTaskShaderWork;
    command.groupCountZ = 1;
    dispatch[0] = command;
}