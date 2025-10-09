import "Scene/Structs";

[[vk::push_constant]] uint32_t sceneParamsOffset; // in sceneShared
[[vk_binding(0, 0)]]
RWStructuredBuffer<MeshletTaskParams> tasks;
[[vk_binding(0, 1)]]
RWStructuredBuffer<Atomic<uint>> counter;
[[vk_binding(0, 2)]]
ByteAddressBuffer sceneConst;
[[vk_binding(0, 3)]]
ByteAddressBuffer sceneShared;
[[vk_binding(0, 4)]]
ByteAddressBuffer sceneInstance;


