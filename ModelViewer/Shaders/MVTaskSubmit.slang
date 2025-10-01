import "Common/Definitions";
import "Math/Bits";

RWStructuredBuffer<Atomic<uint>> counter;
RWStructuredBuffer<MeshletTaskDispatch> dispatch;

[shader("compute")]
[numthreads(1, 1, 1)]
void taskSubmit() {
    // As per VkPhysicalDeviceMeshShaderPropertiesEXT::maxTaskWorkGroupCount
    // min requirements are (65535,65535,65535)
    // We'll do [ceil(N/65535),min(N,65535),1] dispatches for N tasks - task shaders can read the
    // actual task count from the prepared count buffer
    MeshletTaskDispatch command;
    uint N = counter[0].load();
    command.groupCountX = (N + 65534) / 65535;
    command.groupCountY = min(N, 65535);
    command.groupCountZ = 1;
    dispatch[0] = command;
}