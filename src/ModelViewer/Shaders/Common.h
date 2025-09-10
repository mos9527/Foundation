// https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#VkDrawIndexedIndirectCommand
struct MeshDrawIndirectCmd
{
    uint32_t    indexCount;
    uint32_t    instanceCount;
    uint32_t    firstIndex;
    int32_t     vertexOffset;
    uint32_t    firstInstance;
};
// In GetVertexBuffer()
struct VertexSL {
    uint16_t px, py, pz; // quantized fp16
    uint16_t tp;         // tangent [octa 8+8]
    uint32_t np; // normal packed [snorm octa 15+15, bitangent sign 2]
    uint16_t u, v;       // texcoord fp16
};
// In GetIndexBuffer()
typedef uint32_t IndexSL; // 32-bit index
// In GetPrimitiveDataBuffer
struct PrimitiveMetadata {
    /* Vertex Buffers */
    // Index into GetVertexBuffer()
    int vertexOffset;
    /* --- */
    int indexCount;
    // Index into GetIndexBuffer()
    int indexOffset;
    /* --- */
    float4 sphereBounds; // (x,y,z) center, w radius
};
// In GetInstanceDataBuffer
struct InstanceMetadata {
    uint32_t enabled;
    uint32_t primitiveID;
    /* Other per-instance data */
    float4x4 transform;
};
struct DrawPushConstant
{
    float4x4 viewProj;
};
