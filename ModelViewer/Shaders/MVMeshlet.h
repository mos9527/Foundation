import "Common/Definitions";
import "Math/Bits";
import "Math/Quantization";
import "Math/Quaternion";
struct DrawPushConstant
{
    float4x4 viewProj;
    float time;
    float3 _pad;
};
[[vk::push_constant]] DrawPushConstant draw; // Always uses set 1 binding 0
[[vk_binding(0, 0)]]
StructuredBuffer<MeshletTaskParams> tasks;
[[vk_binding(0, 1)]]
StructuredBuffer<uint> counter;
[[vk_binding(0, 2)]]
ByteAddressBuffer sceneConst;
[[vk_binding(0, 3)]]
ByteAddressBuffer sceneInstance;

