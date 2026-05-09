#pragma once
#include "Mesh.hpp"
#include "Serialization.hpp"
#include "Texture.hpp"

// Components
struct FTransform
{
    float3 transform;
    quat rotation;
    float3 scale;
};
struct FInstance
{
    FTransform transform;
    // Index of @ref FMesh - or a *submesh* in glTF terms
    // Each mesh is guaranteed to use a single material, if any.
    uint32_t meshIndex;
    uint32_t materialIndex;
};
struct FCurveInstance
{
    FTransform transform;
    uint32_t curveIndex;
    uint32_t materialIndex;
};
struct FCamera
{
    FTransform transform;
    float fovY;
    bool lensEnabled{false};
    float sensorHeightMm{36.0f};
    float fStop{2.8f};
    float focusDistance{10.0f};
    uint32_t apertureBlades{0u};
    float apertureRotation{0.0f};
    float apertureRatio{1.0f};
};
// https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#metallic-roughness-material
// https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#additional-textures
// Sentinel value for "no texture bound"
static constexpr uint32_t kInvalidTexture = UINT32_MAX;
enum class FMaterialShaderBlock : uint32_t
{
    Principled = 0,
    Hair = 1,
};

struct FMaterial
{
    // sRGB.
    uint32_t baseColorTexture = kInvalidTexture;
    // sRGB
    uint32_t emissiveTexture = kInvalidTexture;
    // Linear. B channel: metallic, G channel: roughness
    uint32_t metallicRoughnessTexture = kInvalidTexture;
    // Linear. Tangent space XYZ, R [0.0 .. 1.0] to X [-1 .. 1], G [0.0 .. 1.0] to Y [-1 .. 1], B (0.5 .. 1.0] maps to Z
    // (0 .. 1]
    uint32_t normalTexture = kInvalidTexture;
    // Linear. R channel.
    uint32_t transmissionTexture = kInvalidTexture;
    // Linear. A channel.
    uint32_t specularTexture = kInvalidTexture;
    // sRGB. RGB channels.
    uint32_t specularColorTexture = kInvalidTexture;
    // Linear. RG direction in tangent space, B strength.
    uint32_t anisotropyTexture = kInvalidTexture;
    float4 baseColorFactor;
    float4 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float transmissionFactor;
    float ior = 1.5f;
    float specularFactor = 1.0f;
    float3 specularColorFactor{1.0f, 1.0f, 1.0f};
    float anisotropyStrength = 0.0f;
    float anisotropyRotation = 0.0f;
    float subsurfaceFactor = 0.0f;
    float subsurfaceScale = 0.05f;
    float3 subsurfaceColor{1.0f, 1.0f, 1.0f};
    float3 subsurfaceRadius{1.0f, 0.2f, 0.1f};
    FMaterialShaderBlock shaderBlockID = FMaterialShaderBlock::Principled;
    float hairBetaM = 0.3f;
    float hairBetaN = 0.3f;
    float hairAlpha = 2.0f;
};
enum class FLightType : uint32_t
{
    Directional = 0,
    Point = 1,
    Spot = 2,
    Disk = 3,
    Rect = 4,
};
inline constexpr uint32_t kMaxSceneLights = 1024;
struct FLight
{
    FTransform transform;
    FLightType type{FLightType::Directional};
    float3 color{1,1,1};            // Normalized RGB color
    float power{1.0f};              // Radiant power (type-dependent unit)
    float range{0.0f};              // 0 = infinite (directional default)
    float spotInnerConeAngle{0.0f}; // radians
    float spotOuterConeAngle{0.7853981f}; // radians, default ~45 deg
    // Disk/Rect light (half-extents)
    float width{1.0f};
    float height{1.0f};
    bool twoSided{false};
    bool normalize{true};
};
struct FCurvePoint
{
    float3 position;
    float radius;
};
enum class FCurveBasis : uint32_t
{
    Linear = 0,
    Bezier = 1,
    BSpline = 2,
    CatmullRom = 3,
};
enum class FCurveRenderMode : uint32_t
{
    Capsule = 0,
};
struct FCurveSet
{
    Vector<FCurvePoint> points;
    Vector<uint32_t> curveVertexCounts;
    FCurveBasis basis{FCurveBasis::Linear};
    FCurveRenderMode renderMode{FCurveRenderMode::Capsule};
    uint32_t materialIndex{0};

    FCurveSet(Allocator* alloc) : points(alloc), curveVertexCounts(alloc) {}
};
struct FSceneColorManagement
{
    float postExposure{0.0f};
    uint32_t viewLutSdrIndex{1u};
    uint32_t viewLutHdrIndex{1u};
};
enum class FSceneEnvironmentType : uint32_t
{
    Color = 0,
    EnvMap = 1,
};
struct FSceneEnvironment
{
    FSceneEnvironmentType type{FSceneEnvironmentType::Color};
    float3 color{1.0f, 1.0f, 1.0f};
    float strength{0.05f};
    float azimuthOffset{0.0f};
};
static constexpr uint32_t kSceneMagic = fourCC("FSC7");
struct FScene
{
    uint32_t mMagic;

    Vector<FCamera> mCameras;
    Vector<FInstance> mInstances;
    Vector<FCurveInstance> mCurveInstances;
    Vector<FMaterial> mMaterials;
    Vector<FMesh> mMeshes;
    Vector<FCurveSet> mCurves;
    Vector<FTexture> mTextures;
    FTexture mViewLutSdr;
    FTexture mViewLutHdr;
    FSceneColorManagement mColorManagement;
    FSceneEnvironment mEnvironment;
    FTexture mEnvironmentMap;
    Vector<FLight> mLights;

    FScene(Allocator* alloc) :
        mMagic(kSceneMagic), mCameras(alloc), mInstances(alloc), mCurveInstances(alloc), mMaterials(alloc),
        mMeshes(alloc), mCurves(alloc), mTextures(alloc), mViewLutSdr(alloc), mViewLutHdr(alloc),
        mEnvironmentMap(alloc), mLights(alloc)
    {
    }
};

/**
 * Loads a GLTF 2.0 file into @ref FScene, with requisite optimizations applied
 * so that uploading can be just memcpy.
 */
void LoadGLTF(StringView path, FScene& scene);
/**
 * Wrapper for @ref FDeserialize
 */
void LoadFSCN(StringView path, FScene& scene);
/**
 * Loads a scene from a path, inferring format from extension
 */
void LoadScene(StringView path, FScene& scene);
/* -- Serialization -- */
template <>
inline void FSerialize(FWriter& w, FCurveSet const& obj)
{
    FSerialize(w, obj.points);
    FSerialize(w, obj.curveVertexCounts);
    FSerialize(w, obj.basis);
    FSerialize(w, obj.renderMode);
    FSerialize(w, obj.materialIndex);
}
template <>
inline void FDeserialize(FReader& r, FCurveSet& obj)
{
    FDeserialize(r, obj.points);
    FDeserialize(r, obj.curveVertexCounts);
    FDeserialize(r, obj.basis);
    FDeserialize(r, obj.renderMode);
    FDeserialize(r, obj.materialIndex);
}
template <>
inline void FSerialize(FWriter& w, FScene const& obj)
{
    FSerialize(w, obj.mMagic);
    FSerialize(w, obj.mCameras);
    FSerialize(w, obj.mInstances);
    FSerialize(w, obj.mCurveInstances);
    FSerialize(w, obj.mMaterials);
    FSerialize(w, obj.mMeshes);
    FSerialize(w, obj.mCurves);
    FSerialize(w, obj.mTextures);
    FSerialize(w, obj.mViewLutSdr);
    FSerialize(w, obj.mViewLutHdr);
    FSerialize(w, obj.mColorManagement);
    FSerialize(w, obj.mEnvironment);
    FSerialize(w, obj.mEnvironmentMap);
    FSerialize(w, obj.mLights);
}
template <>
inline void FDeserialize(FReader& r, FScene& obj)
{
    FDeserialize(r, obj.mMagic);
    CHECK(obj.mMagic == kSceneMagic);
    FDeserialize(r, obj.mCameras);
    FDeserialize(r, obj.mInstances);
    FDeserialize(r, obj.mCurveInstances);
    FDeserialize(r, obj.mMaterials);
    FDeserialize(r, obj.mMeshes, obj.mMeshes.get_allocator().mResource);
    FDeserialize(r, obj.mCurves, obj.mCurves.get_allocator().mResource);
    FDeserialize(r, obj.mTextures, obj.mTextures.get_allocator().mResource);
    FDeserialize(r, obj.mViewLutSdr);
    FDeserialize(r, obj.mViewLutHdr);
    FDeserialize(r, obj.mColorManagement);
    FDeserialize(r, obj.mEnvironment);
    FDeserialize(r, obj.mEnvironmentMap);
    FDeserialize(r, obj.mLights);
}
