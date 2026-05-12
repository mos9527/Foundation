#pragma once
#include "Mesh.hpp"
#include "Curve.hpp"
#include "Serialization.hpp"
#include "Texture.hpp"

// Components
struct FTransform
{
    float3 transform;
    quat rotation;
    float3 scale;
};
enum class FInstanceType : uint32_t
{
    Mesh = 0,
    Curve = 1,
};
struct FInstance
{
    FTransform transform;
    FInstanceType type{FInstanceType::Mesh};
    // Index into the serialized table tagged by @ref type. Mesh means @ref FSerializedMesh, Curve means @ref FSceneCurveDesc.
    uint32_t resourceIndex{0};
    uint32_t materialIndex{0};
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

enum class FSceneEnvironmentType : uint32_t
{
    Color = 0,
    EnvMap = 1,
};
struct FSceneGlobals
{
    FSceneEnvironmentType type{FSceneEnvironmentType::Color};
    float3 color{1.0f, 1.0f, 1.0f};
    float strength{0.05f};
    float azimuthOffset{0.0f};
    float postExposure{0.0f};
    uint32_t viewLutSdrIndex{1u};
    uint32_t viewLutHdrIndex{1u};
};
static constexpr uint32_t kSceneMagic = fourCC("FSCN");
struct FSceneTables
{
    FSceneGlobals globals;
    Vector<FCamera> cameras;
    Vector<FLight> lights;
    Vector<FInstance> instances;
    Vector<FMaterial> materials;
    Vector<FSerializedMesh> meshes;
    Vector<FSerializedCurve> curves;
    Vector<FSerializedTexture> textures;

    explicit FSceneTables(Allocator* alloc = GLOBAL_ALLOC)
        : cameras(alloc), lights(alloc), instances(alloc), materials(alloc), meshes(alloc), curves(alloc), textures(alloc)
    {
    }
};

template <>
inline void FSerialize(FWriter& writer, FSerializedMesh const& mesh)
{
    FSerialize(writer, mesh.vertices);
    FSerialize(writer, mesh.vertexCount);
    FSerialize(writer, mesh.lods);
    FSerialize(writer, mesh.dagGroups);
    FSerialize(writer, mesh.dagMeshlets);
    FSerialize(writer, mesh.dagMeshletTri);
    FSerialize(writer, mesh.dagMeshletVtx);
}

template <>
inline void FDeserialize(FReader& reader, FSerializedMesh& mesh)
{
    FDeserialize(reader, mesh.vertices);
    FDeserialize(reader, mesh.vertexCount);
    FDeserialize(reader, mesh.lods);
    FDeserialize(reader, mesh.dagGroups);
    FDeserialize(reader, mesh.dagMeshlets);
    FDeserialize(reader, mesh.dagMeshletTri);
    FDeserialize(reader, mesh.dagMeshletVtx);
}

template <>
inline void FSerialize(FWriter& writer, FSceneTables const& tables)
{
    FSerialize(writer, tables.globals);
    FSerialize(writer, tables.cameras);
    FSerialize(writer, tables.lights);
    FSerialize(writer, tables.instances);
    FSerialize(writer, tables.materials);
    FSerialize(writer, tables.meshes);
    FSerialize(writer, tables.curves);
    FSerialize(writer, tables.textures);
}

template <>
inline void FDeserialize(FReader& reader, FSceneTables& tables)
{
    FDeserialize(reader, tables.globals);
    FDeserialize(reader, tables.cameras);
    FDeserialize(reader, tables.lights);
    FDeserialize(reader, tables.instances);
    FDeserialize(reader, tables.materials);
    FDeserialize(reader, tables.meshes, tables.meshes.get_allocator().mResource);
    FDeserialize(reader, tables.curves);
    FDeserialize(reader, tables.textures);
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
    uint32_t reserved{0};
};

struct FScene
{
    // Resident metadata in memory. Payload blobs stay in the external reader/writer.
    FSceneHeader mHeader{};
    FSceneTables mTables{};
    // Non-owning IO view where payload blobs are stored. Reader/writer lifetime is owned by the caller.
    // Mutually exclusive.
    FReader* mReader{nullptr};
    FWriter* mWriter{nullptr};

    // Constructs append-only FScene view over a writer. The writer must outlive this scene.
    FScene(FWriter& writer);
    // Constructs read-only FScene view over a reader. The reader must outlive this scene.
    FScene(FReader& reader);
    ~FScene();

    // Append-only operations
    void Set(FSceneGlobals const& globals)
    {
        CHECK(mWriter != nullptr || mReader != nullptr);
        mTables.globals = globals;
    }
    void Add(FCamera const& camera)
    {
        CHECK(mWriter != nullptr);
        mTables.cameras.push_back(camera);
    }
    void Add(FLight const& light)
    {
        CHECK(mWriter != nullptr);
        mTables.lights.push_back(light);
    }
    void Add(FInstance const& instance)
    {
        CHECK(mWriter != nullptr);
        mTables.instances.push_back(instance);
    }
    void Add(FMaterial const& material)
    {
        CHECK(mWriter != nullptr);
        mTables.materials.push_back(material);
    }
    void Add(FSerializedMesh const& mesh)
    {
        CHECK(mWriter != nullptr);
        mTables.meshes.push_back(mesh);
    }
    void Add(FSerializedCurve const& curve)
    {
        CHECK(mWriter != nullptr);
        mTables.curves.push_back(curve);
    }
    void Add(FSerializedTexture const& texture)
    {
        CHECK(mWriter != nullptr);
        mTables.textures.push_back(texture);
    }

    // Reader only operations
    // Resident data changes are local, and never reflected in the file
    FSceneGlobals const& GetSceneGlobals() const { return mTables.globals; }
    FSceneGlobals& GetSceneGlobals() { return mTables.globals; }

    Span<FCamera const> GetCameras() const { return {mTables.cameras.data(), mTables.cameras.size()}; }
    Span<FCamera> GetCameras() { return {mTables.cameras.data(), mTables.cameras.size()}; }

    Span<FLight const> GetLights() const { return {mTables.lights.data(), mTables.lights.size()}; }
    Span<FLight> GetLights() { return {mTables.lights.data(), mTables.lights.size()}; }

    Span<FInstance const> GetInstances() const { return {mTables.instances.data(), mTables.instances.size()}; }
    Span<FInstance> GetInstances() { return {mTables.instances.data(), mTables.instances.size()}; }

    Span<FMaterial const> GetMaterials() const { return {mTables.materials.data(), mTables.materials.size()}; }
    Span<FMaterial> GetMaterials() { return {mTables.materials.data(), mTables.materials.size()}; }

    Span<FSerializedMesh const> GetMeshes() const { return {mTables.meshes.data(), mTables.meshes.size()}; }
    Span<FSerializedCurve const> GetCurves() const { return {mTables.curves.data(), mTables.curves.size()}; }
    Span<FSerializedTexture const> GetTextures() const { return {mTables.textures.data(), mTables.textures.size()}; }

    FBlobDeserializer GetBlobDeserializer() const;
    bool ReadBlob(FBlobRef const& blob, void* dst, size_t size) const;
    bool ReadBlobRange(FBlobRef const& blob, uint64_t srcOffset, void* dst, size_t size) const;

    template <typename T>
    Vector<T> ReadBlobArray(FBlobRef const& blob, Allocator* alloc = GLOBAL_ALLOC) const
    {
        return GetBlobDeserializer().ReadArray<T>(blob, alloc);
    }
};

void LoadGLTF(StringView path, FScene& scene);
void LoadFSCN(FReader& reader, FScene& scene);

/**
 * Loads a scene from a path, inferring format from extension.
 * Returns the FSCN path that backs payload blob reads.
 */
String LoadScene(StringView path, FScene& scene);
