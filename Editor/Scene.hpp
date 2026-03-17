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
};
struct FLight
{
    FTransform transform;
    float3 color;
    // Directional: Lux, Point: Lumen
    float intensity;
};
static constexpr uint32_t kSceneMagic = fourCC("FSCN");
struct FScene
{
    uint32_t mMagic;

    Vector<FCamera> mCameras;
    Vector<FInstance> mInstances;
    Vector<FMaterial> mMaterials;
    Vector<FMesh> mMeshes;
    Vector<FTexture2D> mTextures;
    Vector<FLight> mLights;
    // Optional environment map
    Optional<FTexture2D> mEnvMap;

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
    FSerialize(w, obj.mEnvMap);
}
template <>
inline void FDeserialize(FReader& r, FScene& obj)
{
    FDeserialize(r, obj.mMagic);
    CHECK(obj.mMagic == kSceneMagic);
    FDeserialize(r, obj.mCameras);
    FDeserialize(r, obj.mInstances);
    FDeserialize(r, obj.mMaterials);
    FDeserialize(r, obj.mMeshes, obj.mMeshes.get_allocator().mResource);
    FDeserialize(r, obj.mTextures, obj.mTextures.get_allocator().mResource);
    FDeserialize(r, obj.mLights);
    FDeserialize(r, obj.mEnvMap, obj.mTextures.get_allocator().mResource);
}
