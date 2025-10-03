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

struct MeshletPayload
{
    uint32_t meshletRawOffsets[kMaxTaskShaderWork];
    uint32_t meshletCount;
    uint32_t instanceID;
    uint32_t meshletVerticesRawOffset;
    uint32_t meshletTrianglesRawOffset;
};
// NV doesn't support 16bit storage buffers
struct MeshVertexCompact32 {
    uint32_t pxy;
    uint32_t pztp;
    uint32_t np;
    uint32_t uv;
};
struct MeshVertexCompact {
    uint16_t px, py, pz; // quantized fp16
    uint16_t tp;         // tangent [octa 8+8]
    uint32_t np;         // normal packed [snorm octa 15+15, bitangent sign 2]
    uint16_t u, v;       // texcoord fp16
};
struct VertexOutput
{
    float4 pos;
    float3 normal;
    float3 tangent;
    float3 bitangent;
    float2 uv;
    int instance;
    int meshlet;
};
