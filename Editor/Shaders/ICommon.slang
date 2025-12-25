import IMath;
public static const uint kMeshWorkGroupSize = 64;
public static const uint kMeshIndirectPerDrawMeshlets = 65536;
// Definitions From C++ sources
public struct FVertex {
    public float3 position;
    public float3 normal;
    public float3 tangent;
    public float bitangentSign;
    public float2 uv;
};
public struct FQVertex {
    public half4 position;
    public uint32_t tbn32;
    public half2 uv;
};
public void FQDecode(in FQVertex v, out float3 position, out float3 normal, out float3 tangent, out float bitangentSign, out float2 uv) {
    position = float3(v.position.xyz);
    unpackTBN(v.tbn32, normal, tangent, bitangentSign);
    uv = float2(v.uv);
}
public void FQDecode(in FQVertex v, out float3 normal, out float3 tangent, out float bitangentSign, out float2 uv) {
    unpackTBN(v.tbn32, normal, tangent, bitangentSign);
    uv = float2(v.uv);
}
public struct FLODGroup // @ref clodGroup
{
    // DAG level the group was generated at
    public int depth;

    // sphere bounds, in mesh coordinate space
    public float3 center;
    public float radius;

    // combined simplification error, in mesh coordinate space
    // FLT_MAX for terminal groups
    public float error;
};
public struct FMeshlet // @ref meshopt_Meshlet
{
    /* meshlet group */
    /* ID of the @ref FLODGroup this meshlet belongs to in a hierarchy */
    public uint32_t group;
    /* ID of the @ref FLODGroup (with more triangles) that produced this meshlet during simplification (parent). ~0u if original geometry */
    public uint32_t refined;

    /* meshlet */
    /* offsets within meshletVtx and meshletTri arrays with meshlet data */
    public uint32_t vtxOffset; // in vertices
    public uint32_t triOffset; // in indices (3*triangles)
    /* number of vertices and triangles used in the meshlet; data is stored in consecutive range defined by offset and
     * count */
    public uint32_t vtxCount;
    public uint32_t triCount;

    /* bounds */
    public float4 centerRadius; // (x,y,z,r)
    public float4 coneAxisAngle; // (x,y,z,cos(half solid angle))
    public float3 coneApex; // (x,y,z)
};
public struct GSMesh
{
    // Offsets are absolute, and are in Primitive buffer (bytes).
    // @ref FVertex
    public uint32_t vtxOffset;
    public uint32_t vtxCount;
    // LOD0 UINT32 indices
    public uint32_t idxOffset;
    public uint32_t idxCount;
    // -- DAG LOD Group @ref FLODGroup
    public uint32_t groupOffset;
    public uint32_t groupCount;
    // -- DAG Meshlets @ref FMeshlet
    public uint32_t meshletOffset;
    public uint32_t meshletCount;
    public uint32_t meshletVtxOffset;
    public uint32_t meshletTriOffset;
    // -- Global index (amongst all loaded meshes)
    public uint32_t meshletGlobalIndex;
};
public struct MIndex {
    public uint8_t v0;
    public uint8_t v1;
    public uint8_t v2;
}
public struct GSInstance
{
    // TRS
    public float3 transform;
    public quat rotation;
    public float3 scale;
    public uint32_t meshOffset; // In Primitive buffer (bytes)
    public uint32_t materialIndex; // In Material buffer (offset)
};
public struct GSMaterial
{
    public uint32_t baseColorTexture;
    public uint32_t emissiveTexture;
    public uint32_t metallicRoughnessTexture;
    public uint32_t normalTexture;
    public float4 baseColorFactor;
    public float3 emissiveFactor;
    public float metallicFactor;
    public float roughnessFactor;
    public float anisotropy;
};
public struct MeshletTaskDispatch // VkDrawMeshTasksIndirectCommandEXT
{
    public uint32_t groupCountX;
    public uint32_t groupCountY;
    public uint32_t groupCountZ;
};
public struct MeshletTaskWork
{
    public uint32_t instanceID; // Absolute
    // We're not using task shaders - but with a dedicated compute shader
    // to further cull meshlets and generate indirect meshlet dispatches.
    // In principle they work the same, however with no GS memory woes and
    // actually works on AMD proprietary drivers.
    // Sigh.
    public uint32_t firstMeshlet;
    public uint32_t numMeshlets;
};
public struct UBO
{
    public uint32_t frameNumber;
    public uint32_t firstInstance;
    public uint32_t numInstances;
    public uint32_t firstMaterial;
    public uint32_t numMaterials;
    public float lodThreshold;
    public float zNear;
    public float4x4 view;
    public float4x4 proj;
    public float4x4 inverseViewProj;
    public float4 projPlanes; // ij:left, kl:top
    public float fbWidth;
    public float fbHeight;
    public uint32_t hizLevels;
    public uint32_t hizWidth;
    public uint32_t hizHeight;
    // -- Lighting
    public float camMinEV;
    public float camMaxEV;
    public float camAdaptCoeff;
    public float3 camPosition;
    public float3 camDirection;
    public float3 sunDirection;
    public float sunIntensity;
    public float3 ambientColor;
    // -- Path Tracing
    public uint32_t ptAccumualatedFrames;
    public uint32_t ptMaxBounces;
};
// ^^^

public struct V2F
{
    public float4 pos : SV_POSITION;
    public float3 normal : NORMAL;
    public float3 tangent : TANGENT;
    public float3 bitangent : BITANGENT;
    public float bitangentSign : BITSIGN;
    public float2 uv : TEXCOORD0;
    public uint32_t meshlet : ID;
    public uint32_t materialIndex : MATID;
};
