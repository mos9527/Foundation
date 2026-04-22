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
// NOTE: GS -> GPU Scene
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
    public uint32_t meshIndex; // Debug use
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
    public float transmissionFactor;
    public float ior;
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
public struct GSLight
{
    public uint32_t type;       // 0=Directional, 1=Point, 2=Spot, 3=Disk, 4=Rect
    public float3 color;        // Normalized RGB color
    public float power;         // Radiant power (type-dependent unit)
    public float3 position;
    public float3 direction;
    public float range;
    public float spotInnerCosAngle;
    public float spotOuterCosAngle;
    // Area lights (Disk / Rect)
    public float3 dpdu;         // tangent u-axis (Rect: half-extent u)
    public float3 dpdv;         // tangent v-axis (Rect: half-extent v)
    public float2 radius;       // Disk radius (x, y for ellipse)
    public uint32_t twoSided;
    public float selectionWeight;

    public float GetSelectionWeight(uint ptLightSampler)
    {
        float weight = 1.0f;
        if (ptLightSampler == 1) { // Power
            float luminance = 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
            weight = luminance * power;
            if (type == 3)
                weight *= radius.x * radius.y * Pi * (twoSided != 0 ? 2.0f : 1.0f);
            else if (type == 4)
                weight *= length(cross(dpdu, dpdv)) * 4.0f * (twoSided != 0 ? 2.0f : 1.0f);
        }
        return max(0.0f, weight);
    }
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
    public float4x4 inverseView;
    public float4x4 inverseViewProj;
    public float4 projPlanes; // ij:left, kl:top
    public float aperture;
    public float focalDistance;
    public float fbWidth;
    public float fbHeight;
    public uint32_t hizLevels;
    public uint32_t hizWidth;
    public uint32_t hizHeight;
    // -- Lighting
    public float camEV;
    public float4 camPosition;
    public float4 camDirection;
    // Environment light
    public float3 ambientColor;
    public float ambientPower;
    public uint32_t useEnvMap;
    public float envMapScale;
    public float envAzimuthOffset;
    // Scene lights
    public uint32_t firstLight;
    public uint32_t firstLightAliasTable;
    public uint32_t numSceneLights;
    public float sceneLightWeightSum;
    public uint32_t _lightPad0;
    // -- Path Tracing
    public uint32_t ptAccumulatedFrames;
    public uint32_t ptMaxBouncesDiffuse;
    public uint32_t ptMaxBouncesSpecular;
    public uint32_t ptMaxBouncesTransmission;
    public float ptFireflyClamp;
    public uint32_t ptSampler;
    public uint32_t ptLightSampler;
    // -- Display
    public uint32_t enableHDR;
    public float paperWhiteNits;
    // -- Debug
    public uint32_t postShowOutline;
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
    public uint32_t instanceID : INSTID;
};