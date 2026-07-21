import IMath;
public static const uint kMeshWorkGroupSize = 64;
public static const uint kMeshIndirectPerDrawMeshlets = 65536;
public static const uint kMaterialShaderBlockPrincipled = 0;
public static const uint kMaterialShaderBlockHair = 1;
public static const uint kCameraProjectionPerspective = 0;
public static const uint kCameraProjectionPanoramic = 1;
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
public FQVertex FQPack(float3 position, float3 normal, float3 tangent, float bitangentSign, float2 uv) {
    FQVertex v;
    v.position = half4((half3)position, (half)0.0);
    v.tbn32 = packTBN(normal, tangent, bitangentSign);
    v.uv = (half2)uv;
    return v;
}
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
public struct GSOffsetCount
{
    public uint32_t offset;
    public uint32_t count;
};
public struct GSMesh
{
    public GSOffsetCount vertices;
    public GSOffsetCount indices;
    public GSOffsetCount groups;
    public GSOffsetCount meshlets;
    public uint32_t meshletVtxOffset;
    public uint32_t meshletTriOffset;
    public uint32_t meshletGlobalIndex;
};
public struct MIndex {
    public uint8_t v0;
    public uint8_t v1;
    public uint8_t v2;
}

public static const uint32_t kGSInstanceTypeMask = 0xFFu;
public static const uint32_t kGSInstanceFlagDynamic = 0x100u;
public static const uint32_t kGSLightTypeMask = 0xFFu;
public static const uint32_t kGSLightTypeEnvironment = 0u;
public static const uint32_t kGSLightTypeDirectional = 1u;
public static const uint32_t kGSLightTypePoint = 2u;
public static const uint32_t kGSLightTypeSpot = 3u;
public static const uint32_t kGSLightTypeDisk = 4u;
public static const uint32_t kGSLightTypeRect = 5u;
public static const uint32_t kGSLightFlagTwoSided = 0x100u;
public static const uint32_t kGSLightFlagUseShadow = 0x200u;
public static const uint32_t kGSLightFlagEnvironmentMap = 0x400u;
public static const uint32_t kLightSamplerBVH = 0u;
public static const uint32_t kLightSamplerUniform = 1u;
public struct GSInstance
{
    // TRS
    public float3 transform;
    public quat rotation;
    public float3 scale;
    public uint32_t resourceOffset; // Mesh or curve set offset in the (static or dynamic) Primitive buffer (bytes)
    public uint32_t materialIndex; // In Material buffer (offset)
    public uint32_t resourceIndex; // Debug use
    public uint32_t type; // [7:0] 0=Mesh, 1=Curve; bit 8 = dynamic (see kGSInstanceFlag*)
};
// Geometry kind (mesh/curve), ignoring feature flags.
public uint32_t GSInstanceType(GSInstance i) { return i.type & kGSInstanceTypeMask; }
// True when the instance's primitive data lives in the dynamic (CPU-updateable) ring buffer.
public bool GSInstanceIsDynamic(GSInstance i) { return (i.type & kGSInstanceFlagDynamic) != 0u; }
public struct GSCurveSet
{
    public GSOffsetCount vertices;
    public GSOffsetCount indices;
    public GSOffsetCount leaves;
};
public struct GSCurveDOTSVertex
{
    public float3 position;
    public float pad;
};
public struct GSCurveLeaf
{
    public float3 p0;
    public float r0;
    public float3 p1;
    public float r1;
    public float u0;
    public float u1;
};
public struct GSMaterial
{
    public uint32_t baseColorTexture;
    public uint32_t emissiveTexture;
    public uint32_t metallicRoughnessTexture;
    public uint32_t normalTexture;
    public float normalScale;
    public uint32_t transmissionTexture;
    public uint32_t specularTexture;
    public uint32_t specularColorTexture;
    public uint32_t anisotropyTexture;
    public uint32_t sheenColorTexture;
    public uint32_t sheenRoughnessTexture;
    public uint32_t clearcoatTexture;
    public uint32_t clearcoatRoughnessTexture;
    public float4 baseColorFactor;
    public float3 emissiveFactor;
    public float metallicFactor;
    public float roughnessFactor;
    public float transmissionFactor;
    public float ior;
    public float specularFactor;
    public float3 specularColorFactor;
    public float anisotropyStrength;
    public float anisotropyRotation;
    public float3 sheenColorFactor;
    public float sheenRoughnessFactor;
    public float clearcoatFactor;
    public float clearcoatRoughnessFactor;
    public float subsurfaceFactor;
    public float subsurfaceScale;
    public float3 subsurfaceColor;
    public float3 subsurfaceRadius;
    public uint32_t shaderBlockID;
    public float hairBetaM;
    public float hairBetaN;
    public float hairAlpha;
};
public struct MeshletTaskDispatch // VkDrawMeshTasksIndirectCommandEXT
{
    public uint32_t groupCountX;
    public uint32_t groupCountY;
    public uint32_t groupCountZ;
};
// Mirrors VkDrawIndexedIndirectCommand (used to GPU-drive the dynamic-geometry raster draw).
public struct DrawIndexedIndirectCommand
{
    public uint32_t indexCount;
    public uint32_t instanceCount;
    public uint32_t firstIndex;
    public int32_t vertexOffset;
    public uint32_t firstInstance; // carries the absolute scene instance index (read via SV_InstanceID)
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
    public uint32_t flags;      // type in low byte; see kGSLightFlag*
    public float3 color;        // Normalized RGB color
    public float power;         // Radiant power (type-dependent unit)
    public float3 position;
    public float3 direction;
    // Directional: x=angularDiameter. Point: x=radius. Spot: x=radius, y=innerCos, z=outerCos.
    // Disk: xy=radius. Environment: x=azimuthOffset.
    public float4 params;
    public float3 dpdu;         // tangent u-axis (Rect: half-extent u)
    public float3 dpdv;         // tangent v-axis (Rect: half-extent v)
};
public uint32_t GSLightType(GSLight light) {
    return light.flags & kGSLightTypeMask;
}
public bool GSLightHasFlag(GSLight light, uint32_t flag) {
    return (light.flags & flag) != 0u;
}
public float ComputeLightProposalWeight(GSLight light) {
    float colorWeight = dot(abs(light.color), float3(1.0f / 3.0f));
    float radiometricWeight = max(0.0f, light.power) * colorWeight;
    uint type = GSLightType(light);
    if (type == kGSLightTypeDirectional)
        return radiometricWeight;
    if (type == kGSLightTypeEnvironment) {
        float averageRadiance = light.params.y > 0.0f ? light.params.y : 1.0f;
        return radiometricWeight * averageRadiance * Pi;
    }
    if (type == kGSLightTypePoint)
        return radiometricWeight * (4.0f * Pi);
    if (type == kGSLightTypeSpot) {
        float cosInner = clamp(max(light.params.y, light.params.z), -1.0f, 1.0f);
        float cosOuter = clamp(min(light.params.y, light.params.z), -1.0f, cosInner);
        float solidAngle = 2.0f * Pi * ((1.0f - cosInner) + (cosInner - cosOuter) / 5.0f);
        return radiometricWeight * solidAngle;
    }
    if (type == kGSLightTypeDisk || type == kGSLightTypeRect) {
        float area = type == kGSLightTypeDisk
            ? Pi * light.params.x * light.params.y
            : 4.0f * length(cross(light.dpdu, light.dpdv));
        float sides = GSLightHasFlag(light, kGSLightFlagTwoSided) ? 2.0f : 1.0f;
        return radiometricWeight * area * Pi * sides;
    }
    return 0.0f;
}
public struct UBO
{
    public uint32_t frameNumber;
    public GSOffsetCount instances;
    public GSOffsetCount materials;
    public float lodThreshold;
    public float zNear;
    public float4x4 view;
    public float4x4 proj;
    public float4x4 inverseView;
    public float4x4 inverseViewProj;
    public float4x4 previousViewProj;
    public uint32_t cameraHistoryFrame;
    public float4 projPlanes; // ij:left, kl:top
    public float aperture;
    public float focalDistance;
    public uint32_t apertureBlades;
    public float apertureRotation;
    public float apertureRatio;
    public uint32_t cameraProjection;
    public float fbWidth;
    public float fbHeight;
    public uint32_t hizLevels;
    public uint32_t hizWidth;
    public uint32_t hizHeight;
    // -- Lighting
    public float camEV;
    public float4 camPosition;
    public float4 camDirection;
    public uint32_t ggxLutEIndex;
    public uint32_t ggxLutEavgIndex;
    public uint32_t ggxLutEIORIndex;
    public uint32_t ggxLutEIORavgIndex;
    public uint32_t ggxLutEIORInvIndex;
    public uint32_t ggxLutEIORInvavgIndex;
    public uint32_t sheenLtcIndex;
    public uint32_t envMapTextureIndex;
    public uint32_t envMapMarginalCDFIndex;
    public uint32_t envMapConditionalCDFIndex;
    public uint32_t envMapPrefilteredMips;
    public uint32_t hasEnvMap;
    public float3 envSHCoeffs[9];
    public uint32_t matcapTextureIndex;
    // Scene lights
    public GSOffsetCount lights;
    public GSOffsetCount lightBVHNodes;
    public GSOffsetCount lightBVHLightIndices;
    public uint32_t firstLightBVHBitmask;
    public GSOffsetCount lightBVHGlobalIndices;
    public uint32_t lightBVHValid;
    public GSOffsetCount lightBVHDistantNodes;
    public uint32_t energyCompensation;
    // -- Path Tracing
    public uint32_t ptAccumulatedFrames;
    public uint32_t ptMaxBouncesDiffuse;
    public uint32_t ptMaxBouncesSpecular;
    public uint32_t ptMaxBouncesTransmission;
    public float ptFireflyClamp;
    public uint32_t ptSamplesPerPixel;
    public uint32_t ptPrimaryLightVisibility;
    // -- Debug
    public uint32_t dbgShowOutline;
    public uint32_t dbgViewFlags;
    public uint32_t dbgMaterialFlags;    
    public uint32_t adaptiveMinSamples;
    public float adaptiveThreshold;
    public float rasterRTShadowBias;
};

public struct PostprocessUBO
{
    public float camEV;
    public uint32_t viewLutIndex;
    public uint32_t dbgShowOutline;
    public uint32_t ptAccumulatedFrames;
    public float fbWidth;
    public float fbHeight;
    public uint32_t outlineInstanceId;
    public float renderWidth;
    public float renderHeight;
    public uint32_t dbgViewFlags;
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

public float ApproximateMipLevelFromUVGradient(float2 uvDX, float2 uvDY, uint width, uint height)
{
    float2 textureSize = float2(float(max(width, 1u)), float(max(height, 1u)));
    float2 dx = uvDX * textureSize;
    float2 dy = uvDY * textureSize;
    float rho2 = max(dot(dx, dx), dot(dy, dy));
    return max(0.0f, 0.5f * log2(max(rho2, 1e-12f)));
}

public float3 VisualizeApproximateMipLevel(float mipLevel)
{
    // False-color over mip levels [0, 8]. Black/blue means sharp, yellow/red means coarse.
    float x = saturate(mipLevel / 8.0f);
    return saturate(float3(
        1.5f - abs(4.0f * x - 3.0f),
        1.5f - abs(4.0f * x - 2.0f),
        1.5f - abs(4.0f * x - 1.0f)));
}
