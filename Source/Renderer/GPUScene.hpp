#pragma once
#include <Core/Allocator.hpp>
#include <Core/AllocatorStack.hpp>
#include <Core/JobSystem.hpp>
#include <Core/Logging.hpp>
#include <RenderCore/Bindless.hpp>
#include <RenderCore/ImmediateContext.hpp>
#include <cstddef>
#include "Curve.hpp"
#include "Mesh.hpp"
#include "Precompute.hpp"
#include "Shaders/Flags.h"
#include "Texture.hpp"

using namespace Math;
using namespace RenderCore;
struct RendererUBO;
struct GPUSceneImpl;

// Must match the procedural hit-group bindings in Render/Pathtracer.cpp.
inline constexpr uint32_t kRectLightSBTOffset = 3u;
inline constexpr uint32_t kDiskLightSBTOffset = 4u;
inline constexpr uint32_t kCurveSBTOffset = 5u;
inline constexpr uint32_t kSphereLightSBTOffset = 6u;
inline constexpr uint32_t kGSInstanceTypeMesh = 0u;
inline constexpr uint32_t kGSInstanceTypeCurve = 1u;
inline constexpr uint32_t kGSInstanceTypeMask = 0xFFu;
// Light Flags
inline constexpr uint32_t kGSLightTypeMask = 0xFFu;
inline constexpr uint32_t kGSLightTypeEnvironment = 0u;
inline constexpr uint32_t kGSLightTypeDirectional = 1u;
inline constexpr uint32_t kGSLightTypePoint = 2u;
inline constexpr uint32_t kGSLightTypeSpot = 3u;
inline constexpr uint32_t kGSLightTypeDisk = 4u;
inline constexpr uint32_t kGSLightTypeRect = 5u;

struct GSOffsetCount
{
    uint32_t offset{0u};
    uint32_t count{0u};
};
static_assert(sizeof(GSOffsetCount) == 8);

struct GeometryHandle
{
    uint32_t index{~0u};
    uint32_t version{0};
    [[nodiscard]] bool IsValid() const { return index != ~0u; }
};

struct TextureHandle
{
    uint32_t index{~0u};
    uint32_t version{0};
    bool is3D{false};
    [[nodiscard]] bool IsValid() const { return index != ~0u; }
};

BITMASK_ENUM_BEGIN(GSData, uint8_t)
Mesh = 1 << 0 
BITMASK_ENUM_END()

#pragma pack(push, 1)
struct GSMesh
{
    GSOffsetCount vertices; // absolute byte offset + count in Primitive buffer (@ref FVertex / FQVertex)
    GSOffsetCount indices;  // LOD0 UINT32 indices
    GSOffsetCount groups;   // DAG LOD Group @ref FLODGroup
    GSOffsetCount meshlets; // DAG Meshlets @ref FMeshlet
    uint32_t meshletVtxOffset;
    uint32_t meshletTriOffset;
    uint32_t meshletGlobalIndex;
};
struct GSInstance
{
    // TRS
    float3 transform{0, 0, 0};
    quat rotation{0, 0, 0, 1};
    float3 scale{1, 1, 1};
    uint32_t resourceOffset{UINT32_MAX}; // Mesh or curve set offset in Primitive buffer (bytes)
    uint32_t materialIndex{0u}; // In Material buffer (offset)
    uint32_t resourceIndex{UINT32_MAX}; // CPU side resource index, see @ref Geometry
    uint32_t type{kGSInstanceTypeMesh};
};
// XXX: Uber. Super, even. Surely this works for all our needs...
struct GSMaterial
{
    uint32_t baseColorTexture = UINT32_MAX;
    uint32_t emissiveTexture = UINT32_MAX;
    uint32_t metallicRoughnessTexture = UINT32_MAX;
    uint32_t normalTexture = UINT32_MAX;
    float normalScale = 1.0f;
    uint32_t transmissionTexture = UINT32_MAX;
    uint32_t specularTexture = UINT32_MAX;
    uint32_t specularColorTexture = UINT32_MAX;
    uint32_t anisotropyTexture = UINT32_MAX;
    uint32_t sheenColorTexture = UINT32_MAX;
    uint32_t sheenRoughnessTexture = UINT32_MAX;
    uint32_t clearcoatTexture = UINT32_MAX;
    uint32_t clearcoatRoughnessTexture = UINT32_MAX;
    float4 baseColorFactor;
    float3 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float transmissionFactor;
    float ior;
    float specularFactor;
    float3 specularColorFactor;
    float anisotropyStrength;
    float anisotropyRotation;
    float3 sheenColorFactor;
    float sheenRoughnessFactor;
    float clearcoatFactor;
    float clearcoatRoughnessFactor;
    float subsurfaceFactor;
    float subsurfaceScale;
    float3 subsurfaceColor;
    float3 subsurfaceRadius;
    uint32_t shaderBlockID;
    float hairBetaM;
    float hairBetaN;
    float hairAlpha;
};
struct GSLight
{
    uint32_t flags; // type in low byte; see GSLightFlagsBits
    float3 color; // Normalized RGB color
    float power; // Radiant power (type-dependent unit)
    float3 position;
    float3 direction;
    // Directional: x=angularDiameter. Point: x=radius. Spot: x=radius, y=innerCos, z=outerCos.
    // Disk: xy=radius. Environment: x=azimuthOffset, y=averageRadiance, 0 when solid color
    float4 params;
    float3 dpdu; // tangent u-axis (Rect: half-extent u)
    float3 dpdv; // tangent v-axis (Rect: half-extent v)
};
struct GSCurveSet
{
    GSOffsetCount vertices; // FCurveDOTSVertex
    GSOffsetCount indices;  // uint32_t, 12 indices per leaf
    GSOffsetCount leaves;   // FCurveLeaf
};
#pragma pack(pop)
static_assert(sizeof(GSMesh) == 44);
static_assert(sizeof(GSInstance) == 56);
static_assert(sizeof(GSMaterial) == 192);
static_assert(sizeof(GSLight) == 84);
static_assert(sizeof(GSCurveSet) == 24);
static_assert(sizeof(FCurveDOTSVertex) == 8);
static_assert(sizeof(FCurveLeaf) == 40);

struct GPUSceneDesc
{
    uint32_t primitiveBudget = 16 * (1u << 20); // 16MB
    uint32_t instanceBudget = static_cast<uint32_t>(1e4); // # of GSInstance elements (ring)
    uint32_t materialBudget = static_cast<uint32_t>(1e3); // # of materials (ring)
    uint32_t lightBudget = static_cast<uint32_t>(1e4); // # of lights (ring)
    uint32_t texturesBudget = static_cast<uint32_t>(1e3); // # of textures
    uint32_t geometryBudget = static_cast<uint32_t>(1e4); // # of geometry (ring)
    uint32_t tlasInstanceBudget = static_cast<uint32_t>(1e4); // # of TLAS instances (ring)
    uint32_t tlasBudget = 16 * (1u << 20); // 16MB
    uint32_t tlasScratchBudget = 32 * (1u << 20); // 32MB (ring)
    uint32_t dynamicGeometryBudget = 0; // device-local bytes (0 = disabled)
    uint32_t dynamicStagingBudget = 0; // CPU staging buffer size for dynamic geometry (0 = disabled)
    uint32_t dynamicStagingFramesInFlight = 1; // CPU staging frames in flight
};

class GPUScene
{
public:
    enum class Result
    {
        Ready,
        InProgress,
        InvalidInput,
        InvalidHandle,
        OutOfMemory,
        DecodeFailed,
        Cancelled, 
    };

    [[nodiscard]] static size_t CalculateMeshPrimitiveSize(FSerializedMesh const& src);
    [[nodiscard]] static size_t CalculateCurvePrimitiveSize(FSerializedCurve const& src);

    GPUScene(RHIDevice* device, JobSystem* jobs, Allocator* allocator, GPUSceneDesc const& desc,
             AllocatorStack* frameScratch = nullptr);
    ~GPUScene();

    Result Upload(FBlobDeserializer* blobs, FSerializedMesh const& source, GeometryHandle& outHandle);
    Result Upload(FImportedMesh const& source, GeometryHandle& outHandle);

    /**
     * @brief Allocates a dynamic geometry handle.
     * @param isGpu Whether the dynamic geometry is GPU-local. See @ref UpdateDynamicGeometryCPU, @ref UpdateDynamicGeometryGPU.
     */
    Result Allocate(uint32_t vertexCount, uint32_t indexCount, GeometryHandle& outHandle, bool isGpu = false);
    Result Allocate(uint32_t vertexCount, uint32_t indexCount, uint32_t leafCount, GeometryHandle& outHandle,
                    bool isGpu = false);
    
    Result Upload(FBlobDeserializer* blobs, FSerializedCurve const& source, GeometryHandle& outHandle);
    
    Result Upload(FBlobDeserializer* blobs, FSerializedTexture const& source, TextureHandle& outTexture,
                  const char* debugName = nullptr, bool pinned = false);
    Result Upload(FTexture const& source, TextureHandle& outTexture, const char* debugName = nullptr,
                  bool pinned = false);
    Result UploadEnvironmentMap(FTexture const& source);
    [[nodiscard]] bool HasEnvironmentMap() const { return mEnvMapIndex.IsValid(); }

    Result Upload(RHIBuffer* dst, Span<const unsigned char> data, uint32_t dstOffset = 0);
    
    [[nodiscard]] Result Query(GeometryHandle handle) const;
    [[nodiscard]] Result Query(TextureHandle texture) const;

    void Join();
    [[nodiscard]] Result Poll(size_t timeout);


    void UpdateUBO(RendererUBO& globals) const;

    struct UpdateResult
    {
        GSOffsetCount instances;
        GSOffsetCount materials;
        GSOffsetCount lights;
        struct LightBVH
        {
            uint32_t valid{0u};
            GSOffsetCount nodes;
            GSOffsetCount nodeIndices;
            GSOffsetCount distantNodes;
            GSOffsetCount bitmasks;
            GSOffsetCount lightIndices;
            GSOffsetCount globalLightIndices;
        } lightBVH;
        /* Hashes */
        uint64_t instancesHash{0u};
        uint64_t materialsHash{0u};
        uint64_t lightsHash{0u};
    };

    struct GPUSceneTables
    {        
        Span<GSInstance> instances;
        Span<GSMaterial> materials;
        Span<GSLight> lights;
        GSOffsetCount instanceRange{};
        GSOffsetCount materialRange{};
        GSOffsetCount lightRange{};
    };

    GPUSceneTables BeginScene(uint32_t instanceCount, uint32_t materialCount, uint32_t lightCount);
    void ResolveGeometry(GeometryHandle handle, uint32_t& outPrimitiveOffset, uint32_t& outPrimitiveType,
                         GSInstanceFlags& outPrimitiveFlags) const;
    UpdateResult EndScene(GPUSceneTables& tables);

    struct MemoryStat
    {
        String name;
        size_t bytes;
    };
    void DbgGetMemoryStatistics(Vector<MemoryStat>& outStats) const;
    [[nodiscard]] String DbgGetBufferStatistics() const;
    [[nodiscard]] uint32_t GetLightCapacity() const;

    enum class TLASBuildResult
    {
        Built,
        Empty
    };
    [[nodiscard]] TLASBuildResult BuildTLAS(RHICommandList* cmd, bool update = false);
    [[nodiscard]] bool HasDynamicGeometry() const;
    [[nodiscard]] bool HasCurveGeometry() const;

    void BeginDynamicGeometryUpdate();
    /**
	 * @brief Host-mapped copy. Requires isGpu = false with @ref Allocate, otherwise use UpdateDynamicGeometryGPU.
	 */
    void UpdateDynamicGeometryCPU(GeometryHandle handle, Span<const FQVertex> vertices = {},
                               Span<const uint32_t> indices = {});
    /**
     * @brief Device local writes.Requires isGpu = true with @ref Allocate, otherwise use UpdateDynamicGeometryCPU.
     * @note  This in effect only marks the geometry dirty, and is thread-safe to commit.
     */
    void UpdateDynamicGeometryGPU(GeometryHandle handle, bool updateVertices, bool updateIndices);
    void UpdateDynamicCurveGPU(GeometryHandle handle, bool updateVertices, bool updateIndices, bool updateLeaves);
    void UpdateDynamicCurveCPU(GeometryHandle handle, Span<const FCurveDOTSVertex> vertices = {},
                               Span<const uint32_t> indices = {}, Span<const FCurveLeaf> leaves = {});
    void EndDynamicGeometryUpdate();

    /**
     * @brief Commits Host-side geometry updates through @ref UpdateDynamicGeometryCPU
     *        No-op if no @ref UpdateDynamicGeometryCPU calls were made since the last @ref EndDynamicGeometryUpdate.
     *        GPU-side updates are not required to be committed, as they are already in device-local memory.
     */
    void UploadDynamicGeometryCPU(RHICommandList* cmd);

    void BuildBLAS(RHICommandList* cmd);
    [[nodiscard]] uint32_t GetDynamicRefitCount() const;
    [[nodiscard]] uint32_t GetDynamicRebuildCount() const;

    /**
     * @brief Garbage-collects geometry and textures no longer referenced by the committed
     *        scene tables.
     */
    void Collect();

    [[nodiscard]] GSInstance GetInstance(uint32_t index) const
    {
        CHECK_MSG(index < mCommittedInstances.size(), "GetInstance index {} out of range ({})", index,
                  mCommittedInstances.size());
        return mCommittedInstances[index];
    }
    [[nodiscard]] uint32_t GetInstanceCount() const { return static_cast<uint32_t>(mCommittedInstances.size()); }
    [[nodiscard]] uint32_t InstanceFromTLAS(uint32_t tlasID) const
    {
        if (tlasID >= mTLASInstanceMap.size())
            return UINT32_MAX;
        return mTLASInstanceMap[tlasID];
    }
    [[nodiscard]] GSLight GetLight(uint32_t index) const
    {
        CHECK_MSG(index < mCommittedLights.size(), "GetLight index {} out of range ({})", index,
                  mCommittedLights.size());
        return mCommittedLights[index];
    }
    [[nodiscard]] uint32_t GetLightCount() const { return static_cast<uint32_t>(mCommittedLights.size()); }
    [[nodiscard]] GSMaterial GetMaterial(uint32_t index) const
    {
        CHECK_MSG(index < mCommittedMaterials.size(), "GetMaterial index {} out of range ({})", index,
                  mCommittedMaterials.size());
        return mCommittedMaterials[index];
    }
    [[nodiscard]] uint32_t GetMaterialCount() const { return static_cast<uint32_t>(mCommittedMaterials.size()); }

    /* Geometry */
    [[nodiscard]] RHIBuffer* GetPrimitiveBuffer() const { return mPrimitiveBuffer.Get(); }
    [[nodiscard]] RHIBuffer* GetDynamicPrimitiveBuffer() const;
    [[nodiscard]] RHIBuffer* GetDynamicStagingBuffer() const;
    [[nodiscard]] RHIBuffer* GetInstanceBuffer() const;
    [[nodiscard]] RHIBuffer* GetMaterialBuffer() const;
    [[nodiscard]] RHIBuffer* GetLightBuffer() const;
    [[nodiscard]] RHIBuffer* GetLightBVHNodeBuffer() const;
    [[nodiscard]] RHIBuffer* GetLightBVHLightIndexBuffer() const;
    [[nodiscard]] RHIBuffer* GetLightBVHBitmaskBuffer() const;
    [[nodiscard]] RHIBuffer* GetLightBVHGlobalIndexBuffer() const;
    [[nodiscard]] RHIBuffer* GetLightBVHNodeIndexBuffer() const;
    [[nodiscard]] bool NeedsLightBVHRefit() const;
    [[nodiscard]] uint32_t GetLightBVHRefitLevelCount() const;
    [[nodiscard]] uint32_t GetLightBVHRefitLevelOffset(uint32_t level) const;
    [[nodiscard]] uint32_t GetLightBVHRefitLevelNodeCount(uint32_t level) const;
    [[nodiscard]] uint32_t GetLightBVHFirstNodeIndex() const;
    [[nodiscard]] GSOffsetCount GetLightBVHNodes() const;
    [[nodiscard]] GSOffsetCount GetLightBVHDistantNodes() const;
    /* Textures */
    [[nodiscard]] BindlessPool* GetTexture2DPool();
    [[nodiscard]] BindlessPool* GetTexture3DPool();
    [[nodiscard]] BindlessPool const* GetTexture2DPool() const;
    [[nodiscard]] BindlessPool const* GetTexture3DPool() const;
    [[nodiscard]] RHITexture* GetFoundationDefaultTexture2D() const;
    [[nodiscard]] RHITexture* GetFoundationDefaultTexture2DFloat() const;
    [[nodiscard]] RHIBuffer* GetFoundationDefaultBufferFloat() const { return mFoundationDefaultBufferFloat.Get(); }
    /* AS */
    [[nodiscard]] RHIAccelerationStructure* GetTLAS() const
    {
        return mTLAS.IsValid() ? mTLAS.Get() : nullptr;
    }
    /* Samplers */
    [[nodiscard]] RHIBuffer* GetSobolMatricesBuffer() const { return mSobolMatricesBuffer.Get(); }

    void Reset();

private:
    friend struct GPUSceneImpl;
    UniquePtr<GPUSceneImpl> mImpl;

    UpdateResult mLastUpdateResult;
    // Per-frame commits
    Vector<GSInstance> mCommittedInstances;
    Vector<GSLight> mCommittedLights;
    Vector<GSMaterial> mCommittedMaterials;
    // TLAS instanceID -> committed instance index
    Vector<uint32_t> mTLASInstanceMap;
    // Primitive buffer for geometries
    RHIDeviceScopedHandle<RHIBuffer> mPrimitiveBuffer;
    RHIDeviceScopedHandle<RHIAccelerationStructure> mTLAS;
    uint32_t mLastTLASInstancesCount{0};
    // Shared default resources
    RHIDeviceScopedHandle<RHIBuffer> mSobolMatricesBuffer;
    RHIDeviceScopedHandle<RHIBuffer> mFoundationDefaultBufferFloat;

public:
    // Texture Handles
    TextureHandle mFoundationDefaultTexture2DIndex;
    TextureHandle mFoundationDefaultTexture2DFloatIndex;
    // Precomputed LUTs (texture2D pool)
    TextureHandle mLUTGGXEIndex;
    TextureHandle mLUTGGXEavgIndex;
    TextureHandle mLUTGGXEIORavgIndex;
    TextureHandle mLUTGGXEIORInvavgIndex;
    // Precomputed LUTs (texture3D pool)
    TextureHandle mLUTGGXEIORIndex;
    TextureHandle mLUTGGXEIORInvIndex;
    TextureHandle mLUTSheenLTCIndex;
    // Environment map (texture2D pool)
    TextureHandle mEnvMapIndex;
    TextureHandle mEnvMapMarginalCDFIndex;
    TextureHandle mEnvMapConditionalCDFIndex;
    uint32_t mEnvMapPrefilteredMips{0u};
    Array<float3, 9> mEnvSHCoeffs{};
    float mEnvMapAverageRadiance{1.0f};
};

ENUM_NAME_CONV_BEGIN(GPUScene::Result)
ENUM_NAME(Ready)
ENUM_NAME(InProgress)
ENUM_NAME(InvalidInput)
ENUM_NAME(InvalidHandle)
ENUM_NAME(OutOfMemory)
ENUM_NAME(DecodeFailed)
ENUM_NAME(Cancelled)
ENUM_NAME_CONV_END()
