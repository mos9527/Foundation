#pragma once
#include <Core/JobSystem.hpp>
#include <Renderer/Mesh.hpp>
#include <Renderer/Animation.hpp>
#include <Renderer/Curve.hpp>
#include <Renderer/Serialization.hpp>
#include <Renderer/Texture.hpp>
#include <utility>

namespace Foundation::RHI { struct RHIDeviceCapabilities; }
struct GPUSceneDesc;

// Components
enum class FInstanceType : uint32_t
{
    Mesh = 0,
    Curve = 1,
};
struct FInstance
{
    FTransform transform;
    FInstanceType type{FInstanceType::Mesh};
    FUUID id{};
    FUUID name{}; // FStringEntry id; kNilUUID when unnamed.
    FUUID resource{};
    FUUID material{}; // kDefaultMaterialUUID for implicit default.
};
struct FCamera
{
    FTransform transform;
    FUUID id{};
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
// Well-known default material at materials[0]; replaces legacy "index 0 reserved" + importer +1.
static constexpr FUUID kDefaultMaterialUUID{ .hi = 0x0f0f0f0f0f0f0f0full, .lo = 0x0000000000000001ull };
enum class FMaterialShaderBlock : uint32_t
{
    Principled = 0,
    Hair = 1,
};

struct FMaterial
{
    FUUID id{};
    FUUID name{};
    // sRGB.
    FUUID baseColorTexture{};
    // sRGB
    FUUID emissiveTexture{};
    // Linear. B channel: metallic, G channel: roughness
    FUUID metallicRoughnessTexture{};
    // Linear. Tangent space XYZ, R [0.0 .. 1.0] to X [-1 .. 1], G [0.0 .. 1.0] to Y [-1 .. 1], B (0.5 .. 1.0] maps to Z
    // (0 .. 1]
    FUUID normalTexture{};
    // glTF material.normalTextureInfo.scale; scales normal-map X/Y before TBN application.
    float normalScale = 1.0f;
    // Linear. R channel.
    FUUID transmissionTexture{};
    // Linear. A channel.
    FUUID specularTexture{};
    // sRGB. RGB channels.
    FUUID specularColorTexture{};
    // Linear. RG direction in tangent space, B strength.
    FUUID anisotropyTexture{};
    // sRGB. RGB channels.
    FUUID sheenColorTexture{};
    // Linear. A channel per KHR_materials_sheen.
    FUUID sheenRoughnessTexture{};
    // Linear. R channel per KHR_materials_clearcoat.
    FUUID clearcoatTexture{};
    // Linear. G channel per KHR_materials_clearcoat.
    FUUID clearcoatRoughnessTexture{};
    // Linear RGB. A is opacity (linear).
    float4 baseColorFactor;
    // Linear RGB. W is intensity multiplier (linear).
    float4 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float transmissionFactor;
    float ior = 1.5f;
    float specularFactor = 1.0f;
    // Linear RGB.
    float3 specularColorFactor{1.0f, 1.0f, 1.0f};
    float anisotropyStrength = 0.0f;
    float anisotropyRotation = 0.0f;
    // Linear RGB.
    float3 sheenColorFactor{0.0f, 0.0f, 0.0f};
    float sheenRoughnessFactor = 0.0f;
    float clearcoatFactor = 0.0f;
    float clearcoatRoughnessFactor = 0.0f;
    float subsurfaceFactor = 0.0f;
    float subsurfaceScale = 0.05f;
    // Linear RGB.
    float3 subsurfaceColor{1.0f, 1.0f, 1.0f};
    float3 subsurfaceRadius{1.0f, 0.2f, 0.1f};
    FMaterialShaderBlock shaderBlockID = FMaterialShaderBlock::Principled;
    float hairBetaM = 0.25f;
    float hairBetaN = 0.5f;
    float hairAlpha = 2.0f;
};
enum class FLightType : uint32_t
{
    Environment = 0,
    Directional = 1,
    Point = 2,
    Spot = 3,
    Disk = 4,
    Rect = 5,
};
inline constexpr uint32_t kMaxSceneLights = 1024;
struct FLight
{
    FTransform transform;
    FUUID id{};
    FUUID name{};
    FLightType type{FLightType::Directional};
    float3 color{1,1,1};            // Linear normalized RGB chromaticity
    float power{1.0f};              // Radiant power (type-dependent unit, linear)
    float angularDiameter{0.0f};    // For directional lights, the apparent size of the light source disk (radians). 0 = punctual. 
    float radius{0.0f};             // Point/Spot emitter sphere radius. 0 = punctual.
    float spotInnerConeAngle{0.0f}; // radians
    float spotOuterConeAngle{0.7853981f}; // radians, default ~45 deg
    bool useShadow{true};
    // Disk/Rect light (half-extents)
    float width{1.0f};
    float height{1.0f};
    bool twoSided{false};
    bool normalize{true};
    bool environmentMap{false};
    FUUID environmentTexture{};
    float environmentAzimuthOffset{0.0f};
    // Scene-node hierarchy index for rigid node animation; -1 when static.
    int32_t node{-1};

    bool HasEnvironmentTexture() const { return environmentMap && !environmentTexture.IsNil(); }
};

inline FLight MakeDefaultEnvironmentLight()
{
    FLight light{};
    light.id = FUUID::Generate();
    light.type = FLightType::Environment;
    light.color = float3{0.05f, 0.05f, 0.05f};
    light.power = 1.0f;
    light.node = -1;
    return light;
}

struct FSceneGlobals
{
    float postExposure{0.0f};
    uint32_t viewLutSdrIndex{1u};
    uint32_t viewLutHdrIndex{1u};
};
static constexpr uint32_t kSceneMagic = fourCC("FSCN");
static constexpr uint32_t kSceneVersion = 24;

// Stringpool entry
struct FStringEntry
{
    FUUID id{};
    String value;
};

struct FSceneTables
{
    FSceneGlobals globals;
    Vector<FStringEntry> strings;
    Vector<FCamera> cameras;
    Vector<FLight> lights;
    Vector<FInstance> instances;
    Vector<FMaterial> materials;
    Vector<FSerializedMesh> meshes;
    Vector<FSerializedCurve> curves;
    Vector<FSerializedTexture> textures;
    Vector<FSkeleton> skeletons;
    Vector<FAnimationClip> clips;
    FUUID sceneNodeSkeleton{}; // kNilUUID when no rigid node hierarchy.

    explicit FSceneTables(Allocator* alloc = GLOBAL_ALLOC)
        : strings(alloc), cameras(alloc), lights(alloc), instances(alloc), materials(alloc), meshes(alloc), curves(alloc),
          textures(alloc), skeletons(alloc), clips(alloc)
    {
    }
};

// id -> table index; rebuilt via FImportedScene::RebuildIndex.
struct FSceneIndex
{
    HashMap<FUUID, uint32_t> strings;
    HashMap<FUUID, uint32_t> instances;
    HashMap<FUUID, uint32_t> materials;
    HashMap<FUUID, uint32_t> lights;
    HashMap<FUUID, uint32_t> textures;
    HashMap<FUUID, uint32_t> meshes;
    HashMap<FUUID, uint32_t> curves;
    HashMap<FUUID, uint32_t> skeletons;

    explicit FSceneIndex(Allocator* alloc = GLOBAL_ALLOC)
        : strings(alloc), instances(alloc), materials(alloc), lights(alloc), textures(alloc),
          meshes(alloc), curves(alloc), skeletons(alloc)
    {
    }

    static int Find(HashMap<FUUID, uint32_t> const& map, FUUID id)
    {
        if (id.IsNil())
            return -1;
        auto it = map.find(id);
        return it == map.end() ? -1 : static_cast<int>(it->second);
    }
};

template <>
inline void FSerialize(FWriter& writer, FSerializedMesh const& mesh)
{
    FSerialize(writer, mesh.id);
    FSerialize(writer, mesh.bounds);
    FSerialize(writer, mesh.vertices);
    FSerialize(writer, mesh.vertexCount);
    FSerialize(writer, mesh.lods);
    FSerialize(writer, mesh.dagGroups);
    FSerialize(writer, mesh.dagMeshlets);
    FSerialize(writer, mesh.dagMeshletTri);
    FSerialize(writer, mesh.dagMeshletVtx);
    FSerialize(writer, mesh.skinBinding);
    FSerialize(writer, mesh.skeleton);
}

template <>
inline void FDeserialize(FReader& reader, FSerializedMesh& mesh)
{
    FDeserialize(reader, mesh.id);
    FDeserialize(reader, mesh.bounds);
    FDeserialize(reader, mesh.vertices);
    FDeserialize(reader, mesh.vertexCount);
    FDeserialize(reader, mesh.lods);
    FDeserialize(reader, mesh.dagGroups);
    FDeserialize(reader, mesh.dagMeshlets);
    FDeserialize(reader, mesh.dagMeshletTri);
    FDeserialize(reader, mesh.dagMeshletVtx);
    FDeserialize(reader, mesh.skinBinding);
    FDeserialize(reader, mesh.skeleton);
}

// FJoint is trivially copyable, so a skeleton's joint array serializes in bulk.
template <>
inline void FSerialize(FWriter& writer, FSkeleton const& skel)
{
    FSerialize(writer, skel.id);
    FSerialize(writer, skel.joints);
}
template <>
inline void FDeserialize(FReader& reader, FSkeleton& skel)
{
    FDeserialize(reader, skel.id);
    FDeserialize(reader, skel.joints);
}

template <>
inline void FSerialize(FWriter& writer, FAnimChannel const& channel)
{
    FSerialize(writer, channel.joint);
    FSerialize(writer, channel.path);
    FSerialize(writer, channel.interp);
    FSerialize(writer, channel.times);
    FSerialize(writer, channel.values);
}
template <>
inline void FDeserialize(FReader& reader, FAnimChannel& channel)
{
    FDeserialize(reader, channel.joint);
    FDeserialize(reader, channel.path);
    FDeserialize(reader, channel.interp);
    FDeserialize(reader, channel.times);
    FDeserialize(reader, channel.values);
}

template <>
inline void FSerialize(FWriter& writer, FAnimationClip const& clip)
{
    FSerialize(writer, clip.id);
    FSerialize(writer, clip.name);
    FSerialize(writer, clip.channels);
    FSerialize(writer, clip.duration);
    FSerialize(writer, clip.skeleton);
}
template <>
inline void FDeserialize(FReader& reader, FAnimationClip& clip)
{
    FDeserialize(reader, clip.id);
    FDeserialize(reader, clip.name);
    FDeserialize(reader, clip.channels, clip.channels.get_allocator().mResource);
    FDeserialize(reader, clip.duration);
    FDeserialize(reader, clip.skeleton);
}

// FStringEntry: id is FUUID::FromString(value).
template <>
inline void FSerialize(FWriter& writer, FStringEntry const& entry)
{
    FSerialize(writer, entry.id);
    FSerialize(writer, entry.value);
}
template <>
inline void FDeserialize(FReader& reader, FStringEntry& entry)
{
    FDeserialize(reader, entry.id);
    FDeserialize(reader, entry.value);
}

template <>
inline void FSerialize(FWriter& writer, FSceneTables const& tables)
{
    FSerialize(writer, tables.globals);
    FSerialize(writer, tables.strings);
    FSerialize(writer, tables.cameras);
    FSerialize(writer, tables.lights);
    FSerialize(writer, tables.instances);
    FSerialize(writer, tables.materials);
    FSerialize(writer, tables.meshes);
    FSerialize(writer, tables.curves);
    FSerialize(writer, tables.textures);
    FSerialize(writer, tables.skeletons);
    FSerialize(writer, tables.clips);
    FSerialize(writer, tables.sceneNodeSkeleton);
}

template <>
inline void FDeserialize(FReader& reader, FSceneTables& tables)
{
    FDeserialize(reader, tables.globals);
    FDeserialize(reader, tables.strings);
    FDeserialize(reader, tables.cameras);
    FDeserialize(reader, tables.lights);
    FDeserialize(reader, tables.instances);
    FDeserialize(reader, tables.materials);
    FDeserialize(reader, tables.meshes, tables.meshes.get_allocator().mResource);
    FDeserialize(reader, tables.curves);
    FDeserialize(reader, tables.textures, tables.textures.get_allocator().mResource);
    FDeserialize(reader, tables.skeletons, tables.skeletons.get_allocator().mResource);
    FDeserialize(reader, tables.clips, tables.clips.get_allocator().mResource);
    FDeserialize(reader, tables.sceneNodeSkeleton);
}

struct FSceneHeader
{
    uint32_t magic{kSceneMagic};
    uint32_t headerSize{0};
    uint64_t metadataOffset{0};
    uint64_t metadataSize{0};
    uint64_t payloadOffset{0};
    uint64_t fileSize{0};
    uint32_t payloadAlignment{16};
    uint32_t version{kSceneVersion};
};

struct FImportedScene
{
    // Resident metadata in memory. Payload blobs stay in the external mapped file.
    FSceneHeader mHeader{};
    FSceneTables mTables;
    FSceneIndex mIndex{GLOBAL_ALLOC};
    // Non-owning mapped file view where payload blobs are stored. Lifetime is owned by the caller.
    MemoryMappedFile* mFile{nullptr};
    Allocator* mScratchAlloc{GLOBAL_ALLOC};
    uint64_t mWriteOffset{0};
    bool mWriting{false};

    // Constructs FImportedScene view over a mapped file. Writable mappings start a new append-only scene.
    explicit FImportedScene(MemoryMappedFile& file, Allocator* scratchAlloc = GLOBAL_ALLOC);
    ~FImportedScene();

    // Append-only operations
    void Set(FSceneGlobals const& globals)
    {
        CHECK(mFile != nullptr);
        mTables.globals = globals;
    }
    void Add(FCamera const& camera)
    {
        CHECK(mWriting);
        mTables.cameras.push_back(camera);
    }
    void Add(FLight const& light)
    {
        CHECK(mWriting);
        mTables.lights.push_back(light);
    }
    void Add(FInstance const& instance)
    {
        CHECK(mWriting);
        mTables.instances.push_back(instance);
    }
    void Add(FMaterial const& material)
    {
        CHECK(mWriting);
        mTables.materials.push_back(material);
    }
    void Add(FSerializedMesh const& mesh)
    {
        CHECK(mWriting);
        mTables.meshes.push_back(mesh);
    }
    void Add(FSerializedCurve const& curve)
    {
        CHECK(mWriting);
        mTables.curves.push_back(curve);
    }
    void Add(FSerializedTexture const& texture)
    {
        CHECK(mWriting);
        mTables.textures.push_back(texture);
    }
    void Add(FSkeleton const& skeleton)
    {
        CHECK(mWriting);
        mTables.skeletons.push_back(skeleton);
    }
    void Add(FAnimationClip const& clip)
    {
        CHECK(mWriting);
        mTables.clips.push_back(clip);
    }

    // Reader only operations
    // Resident data changes are local, and never reflected in the file
    FSceneGlobals const& GetSceneGlobals() const { return mTables.globals; }
    FSceneGlobals& GetSceneGlobals() { return mTables.globals; }

    Span<FCamera const> GetCameras() const { return {mTables.cameras.data(), mTables.cameras.size()}; }
    Span<FCamera> GetCameras() { return {mTables.cameras.data(), mTables.cameras.size()}; }

    Span<FLight const> GetLights() const { return {mTables.lights.data(), mTables.lights.size()}; }
    Span<FLight> GetLights() { return {mTables.lights.data(), mTables.lights.size()}; }
    FLight const* GetEnvironmentLight() const
    {
        if (mTables.lights.empty())
            return nullptr;
        CHECK(mTables.lights.front().type == FLightType::Environment);
        return &mTables.lights.front();
    }
    FLight* GetEnvironmentLight()
    {
        if (mTables.lights.empty())
            return nullptr;
        CHECK(mTables.lights.front().type == FLightType::Environment);
        return &mTables.lights.front();
    }
    FLight& EnsureEnvironmentLight()
    {
        if (mTables.lights.empty())
        {
            // Inserting at the front shifts every light index, so refresh the light lookups.
            mTables.lights.insert(mTables.lights.begin(), MakeDefaultEnvironmentLight());
            RebuildIndex();
        }
        CHECK(mTables.lights.front().type == FLightType::Environment);
        return mTables.lights.front();
    }

    Span<FInstance const> GetInstances() const { return {mTables.instances.data(), mTables.instances.size()}; }
    Span<FInstance> GetInstances() { return {mTables.instances.data(), mTables.instances.size()}; }

    Span<FMaterial const> GetMaterials() const { return {mTables.materials.data(), mTables.materials.size()}; }
    Span<FMaterial> GetMaterials() { return {mTables.materials.data(), mTables.materials.size()}; }

    Span<FSerializedMesh const> GetMeshes() const { return {mTables.meshes.data(), mTables.meshes.size()}; }
    Span<FSerializedCurve const> GetCurves() const { return {mTables.curves.data(), mTables.curves.size()}; }
    Span<FSerializedTexture const> GetTextures() const { return {mTables.textures.data(), mTables.textures.size()}; }
    Span<FSkeleton const> GetSkeletons() const { return {mTables.skeletons.data(), mTables.skeletons.size()}; }
    Span<FAnimationClip const> GetClips() const { return {mTables.clips.data(), mTables.clips.size()}; }
    Span<FStringEntry const> GetStrings() const { return {mTables.strings.data(), mTables.strings.size()}; }

    void RebuildIndex();

    char const* GetName(FUUID id) const
    {
        int const i = FSceneIndex::Find(mIndex.strings, id);
        return i < 0 ? nullptr : mTables.strings[static_cast<size_t>(i)].value.c_str();
    }
    int InstanceIndex(FUUID id) const { return FSceneIndex::Find(mIndex.instances, id); }
    int MaterialIndex(FUUID id) const { return FSceneIndex::Find(mIndex.materials, id); }
    int LightIndex(FUUID id) const { return FSceneIndex::Find(mIndex.lights, id); }
    int TextureIndex(FUUID id) const { return FSceneIndex::Find(mIndex.textures, id); }
    int MeshIndex(FUUID id) const { return FSceneIndex::Find(mIndex.meshes, id); }
    int CurveIndex(FUUID id) const { return FSceneIndex::Find(mIndex.curves, id); }
    int SkeletonIndex(FUUID id) const { return FSceneIndex::Find(mIndex.skeletons, id); }

    // Content-addressed string pool; main-thread only during glTF build.
    FUUID InternString(const char* s)
    {
        if (!s || !*s)
            return kNilUUID;
        FUUID const id = FUUID::FromString(s);
        if (mIndex.strings.find(id) == mIndex.strings.end())
        {
            mIndex.strings.emplace(id, static_cast<uint32_t>(mTables.strings.size()));
            mTables.strings.push_back(FStringEntry{id, s});
        }
        return id;
    }

    Span<const unsigned char> GetPayloadBytes() const;
    FBlobDeserializer GetBlobDeserializer() const;
    bool ReadBlob(FBlobRef const& blob, void* dst, size_t size, Allocator* scratchAlloc) const;
    [[nodiscard]] GPUSceneDesc CalculateGPUSceneDesc(Foundation::RHI::RHIDeviceCapabilities const& caps) const;

    template <typename T>
    Vector<T> ReadBlobArray(FBlobRef const& blob, Allocator* alloc = GLOBAL_ALLOC) const
    {
        return GetBlobDeserializer().ReadArray<T>(blob, alloc);
    }
};

enum class FSceneTextureCompression : uint32_t
{
    BC7 = 0,
    None = 1,
};

struct FSceneBuildOptions
{
#if defined(__ANDROID__)
    FSceneTextureCompression textureCompression{FSceneTextureCompression::None};
#else
    FSceneTextureCompression textureCompression{FSceneTextureCompression::BC7};
#endif
    bool generateMipmaps{true};
    bool generateMeshlets{true};
    bool optimizeMeshes{true};
};

void LoadGLTF(JobSystem* jobs, StringView path, FImportedScene& scene, Allocator* scratchAlloc = GLOBAL_ALLOC,
              FSceneBuildOptions const& buildOptions = FSceneBuildOptions{});
void LoadFSCN(FImportedScene& scene);

/**
 * Loads a scene from a path, inferring format from extension.
 * Returns the FSCN path that backs payload blob reads.
 */
String LoadScene(JobSystem* jobs, StringView path, FImportedScene& scene, Allocator* scratchAlloc = GLOBAL_ALLOC,
                 FSceneBuildOptions const& buildOptions = FSceneBuildOptions{});