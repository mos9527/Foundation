/**
 * @brief Data definition shared between C++ and Slang shaders
 * XXX: It would seem that slang always aligns to 16 bytes
 * https://github.com/shader-slang/slang/discussions/5705
 *
 * TODO: Sharing the same definition between C++ and shaders is usually not
 *       a good idea. MSVC proves it too. Remove this ASAP.
*/

// https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#VkDrawIndexedIndirectCommand
struct MeshDrawIndirectCmd
{
    uint32_t    instanceID;
    uint32_t    indexCount;
    uint32_t    instanceCount;
    uint32_t    firstIndex;
    int32_t     vertexOffset;
    uint32_t    firstInstance;
};
// In GetPrimitiveDataBuffer
struct PrimitiveData {
    /* Vertex Buffers */
    // Index into GetVertexBuffer()
    int vertexOffset;
    /* --- */
    int indexCount;
    // Index into GetIndexBuffer()
    int indexOffset;
};
// In GetInstanceDataBuffer
struct InstanceData {
    /* Raw Offset in Primitive buffer + 1. Set to 0 to disable. */
    uint32_t primitiveOffsetPP;
    /* Other per-instance data */
    // Translation
    float3 t;
    // Rotation quaternion xyzw
    float4 q;
};
struct DrawPushConstant
{
    float4x4 viewProj;
    float time;
    float3 _pad;
};
