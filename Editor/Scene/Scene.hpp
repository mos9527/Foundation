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
struct FCamera
{
    FTransform transform;
    float fovY;
    bool lensEnabled{false};
    float sensorHeightMm{36.0f};
    float fStop{2.8f};
    float focusDistance{10.0f};
};
// https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#metallic-roughness-material
// https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#additional-textures
// Sentinel value for "no texture bound"
static constexpr uint32_t kInvalidTexture = UINT32_MAX;
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
    float4 baseColorFactor;
    float3 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float transmissionFactor;
    float ior = 1.5f;
    float subsurfaceFactor = 0.0f;
    float3 subsurfaceColor{1.0f, 1.0f, 1.0f};
    float3 subsurfaceRadius{1.0f, 1.0f, 1.0f};
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
static constexpr uint32_t kSceneMagicV1 = fourCC("FSCN");
static constexpr uint32_t kSceneMagicV2 = fourCC("FSC2");
static constexpr uint32_t kSceneMagic = fourCC("FSC3");
struct FScene
{
    uint32_t mMagic;

    Vector<FCamera> mCameras;
    Vector<FInstance> mInstances;
    Vector<FMaterial> mMaterials;
    Vector<FMesh> mMeshes;
    Vector<FTexture2D> mTextures;
    Vector<FLight> mLights;

    FScene(Allocator* alloc) :
        mMagic(kSceneMagic), mCameras(alloc), mInstances(alloc), mMaterials(alloc), mMeshes(alloc), mTextures(alloc), mLights(alloc)
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
inline void FSerialize(FWriter& w, FScene const& obj)
{
    FSerialize(w, obj.mMagic);
    FSerialize(w, obj.mCameras);
    FSerialize(w, obj.mInstances);
    FSerialize(w, obj.mMaterials);
    FSerialize(w, obj.mMeshes);
    FSerialize(w, obj.mTextures);
    FSerialize(w, obj.mLights);
}
template <>
inline void FDeserialize(FReader& r, FScene& obj)
{
    FDeserialize(r, obj.mMagic);
    CHECK(obj.mMagic == kSceneMagic || obj.mMagic == kSceneMagicV2 || obj.mMagic == kSceneMagicV1);
    uint32_t sourceMagic = obj.mMagic;
    if (sourceMagic == kSceneMagic)
    {
        FDeserialize(r, obj.mCameras);
    }
    else
    {
        struct FCameraV2
        {
            FTransform transform;
            float fovY;
        };

        uint64_t count = 0;
        FDeserialize(r, count);
        obj.mCameras.resize(count);
        for (size_t i = 0; i < count; ++i)
        {
            FCameraV2 src{};
            FDeserialize(r, src);
            auto& dst = obj.mCameras[i];
            dst.transform = src.transform;
            dst.fovY = src.fovY;
        }
    }
    FDeserialize(r, obj.mInstances);
    if (sourceMagic == kSceneMagicV1)
    {
        struct FMaterialV1
        {
            uint32_t baseColorTexture;
            uint32_t emissiveTexture;
            uint32_t metallicRoughnessTexture;
            uint32_t normalTexture;
            float4 baseColorFactor;
            float3 emissiveFactor;
            float metallicFactor;
            float roughnessFactor;
            float transmissionFactor;
            float ior;
        };

        uint64_t count = 0;
        FDeserialize(r, count);
        obj.mMaterials.resize(count);
        for (size_t i = 0; i < count; ++i)
        {
            FMaterialV1 src{};
            FDeserialize(r, src);
            auto& dst = obj.mMaterials[i];
            dst.baseColorTexture = src.baseColorTexture;
            dst.emissiveTexture = src.emissiveTexture;
            dst.metallicRoughnessTexture = src.metallicRoughnessTexture;
            dst.normalTexture = src.normalTexture;
            dst.baseColorFactor = src.baseColorFactor;
            dst.emissiveFactor = src.emissiveFactor;
            dst.metallicFactor = src.metallicFactor;
            dst.roughnessFactor = src.roughnessFactor;
            dst.transmissionFactor = src.transmissionFactor;
            dst.ior = src.ior;
        }
    }
    else
        FDeserialize(r, obj.mMaterials);
    FDeserialize(r, obj.mMeshes, obj.mMeshes.get_allocator().mResource);
    FDeserialize(r, obj.mTextures, obj.mTextures.get_allocator().mResource);
    FDeserialize(r, obj.mLights);
    obj.mMagic = kSceneMagic;
}
