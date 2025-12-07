#include "IMath.slang"
#include "IFlags.slang"
static const uint kMeshWorkGroupSize = 64;
// Definitions From C++ sources
struct FVertex {
    float3 position;
    float3 normal;
    float3 tangent;
    float bitangentSign;
    float2 uv;
};
struct FQVertex {
    half4 position;
    uint32_t tbn32;
    uint32_t uv;
};
void FQDecode(in FQVertex v, out float3 position, out float3 normal, out float3 tangent, out float bitangentSign, out float2 uv) {
    position = float3(v.position.xyz);
    unpackTBN(v.tbn32, normal, tangent, bitangentSign);
    uv = float2(
        dequantizeSnormShifted(int32_t(bitfieldExtract(v.uv, 0, 16)), 16),
        dequantizeSnormShifted(int32_t(bitfieldExtract(v.uv, 16, 16)), 16)
    );
}
struct FLODGroup // @ref clodGroup
{
    // DAG level the group was generated at
    int depth;

    // sphere bounds, in mesh coordinate space
    float3 center;
    float radius;

    // combined simplification error, in mesh coordinate space
    // FLT_MAX for terminal groups
    float error;
};
struct FMeshlet // @ref meshopt_Meshlet
{
    /* meshlet group */
    /* ID of the @ref FLODGroup this meshlet belongs to in a hierarchy */
    uint32_t group;
    /* ID of the @ref FLODGroup (with more triangles) that produced this meshlet during simplification (parent). ~0u if original geometry */
    uint32_t refined;

    /* meshlet */
    /* offsets within meshletVtx and meshletTri arrays with meshlet data */
    uint32_t vtxOffset; // in vertices
    uint32_t triOffset; // in indices (3*triangles)
    /* number of vertices and triangles used in the meshlet; data is stored in consecutive range defined by offset and
     * count */
    uint32_t vtxCount;
    uint32_t triCount;

    /* bounds */
    float4 centerRadius; // (x,y,z,r)
    float4 coneAxisAngle; // (x,y,z,cos(half solid angle))
    float3 coneApex; // (x,y,z)
};
struct GSMeshLOD
{
    // -- VS pipeline
    uint32_t indOffset;
    uint32_t indCount;
    // -- MS pipeline @ref FMeshlet
    uint32_t meshletOffset;
    uint32_t meshletCount;
    uint32_t meshletVtxOffset;
    uint32_t meshletTriOffset;
};
struct GSMesh
{
    // Offsets are absolute, and are in Primitive buffer (bytes).
    // @ref FVertex
    uint32_t vtxOffset;
    uint32_t vtxCount;
    // -- DAG LOD Group @ref FLODGroup
    uint32_t groupOffset;
    uint32_t groupCount;
    // -- DAG Meshlets @ref FMeshlet
    uint32_t meshletOffset;
    uint32_t meshletCount;
    uint32_t meshletVtxOffset;
    uint32_t meshletTriOffset;
    // -- Global index (amongst all loaded meshes)
    uint32_t meshletGlobalIndex;
};
struct MIndex {
    uint8_t v0;
    uint8_t v1;
    uint8_t v2;
}
struct GSInstance
{
    // TRS
    float3 transform;
    quat rotation;
    float3 scale;
    uint32_t meshOffset; // In Primitive buffer (bytes)
    uint32_t materialIndex; // In Material buffer (offset)
};
struct GSMaterial
{
    uint32_t baseColorTexture;
    uint32_t emissiveTexture;
    uint32_t metallicRoughnessTexture;
    uint32_t normalTexture;
    float4 baseColorFactor;
    float3 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
};
struct MeshletTaskDispatch // VkDrawMeshTasksIndirectCommandEXT
{
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;
};
struct MeshletTaskWork
{
    uint32_t instanceID; // Absolute
    // Task shader can *only* dispatch zero or one meshlet per work thread.
    // Hence, batching is required here. Whereas in Mesh Shader multiple verts/tris
    // can be processed per thread.
    uint32_t firstMeshlet;
    uint32_t numMeshlets;
};
struct UBO
{
    uint32_t firstInstance;
    uint32_t numInstances;
    float lodThreshold;
    float zNear;
    float4x4 view;
    float4x4 proj;
    float4 projPlanes; // ij:left, kl:top
    uint32_t hizLevels;
    uint32_t hizWidth;
    uint32_t hizHeight;
};
// ^^^
public struct MeshletGroupShared {
    uint32_t instanceID;
    uint32_t meshletCount;
    uint32_t meshletIndices[kMeshWorkGroupSize];
};

struct V2F
{
    float4 pos : SV_POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float3 bitangent : BITANGENT;
    float2 uv : TEXCOORD0;
    uint32_t meshlet : ID;
};
