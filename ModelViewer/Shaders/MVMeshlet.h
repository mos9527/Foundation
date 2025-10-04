import "Common/Definitions";
import "Math/Bits";
import "Math/Quantization";
import "Math/Quaternion";

[[vk::push_constant]] CameraParams camera;
[[vk_binding(0, 0)]]
StructuredBuffer<MeshletTaskParams> tasks;
[[vk_binding(0, 1)]]
StructuredBuffer<uint> counter;
[[vk_binding(0, 2)]]
ByteAddressBuffer sceneConst;
[[vk_binding(0, 3)]]
ByteAddressBuffer sceneInstance;

