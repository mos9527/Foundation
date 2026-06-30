#pragma once
#include <Core/Allocator.hpp>
#include <Core/AllocatorStack.hpp>
#include <Core/Logging.hpp>
#include <RenderCore/Bindless.hpp>
#include <RenderCore/ImmediateContext.hpp>
#include <cstddef>
#include "Curve.hpp"
#include "Mesh.hpp"
#include "Precompute.hpp"
#include "Texture.hpp"

using namespace Math;
using namespace RenderCore;
struct RendererUBO;
struct GPUSceneImpl;

// Must match the procedural hit-group bindings in Render/Pathtracer.cpp.
inline constexpr uint32_t kRectLightSBTOffset = 3u;
inline constexpr uint32_t kDiskLightSBTOffset = 4u;
inline constexpr uint32_t kCurveSBTOffset = 5u;
inline constexpr uint32_t kGSInstanceTypeMesh = 0u;
inline constexpr uint32_t kGSInstanceTypeCurve = 1u;
// GSInstance::type low byte is the geometry kind; bit 8 flags CPU-updateable dynamic geometry
// so shaders read its primitive data from the dynamic ring instead of the immutable buffer.
inline constexpr uint32_t kGSInstanceTypeMask = 0xFFu;
inline constexpr uint32_t kGSInstanceFlagDynamic = 0x100u;

/**
 * @brief Opaque, generation-tagged reference to GPUScene-owned geometry residency.
 * @note CPU-only (never uploaded to the GPU). The generation slot catches use of a freed
 *       or recycled handle (@ref GPUScene::Collect).
 */
struct GeometryHandle
{
    uint32_t index{~0u};
    uint32_t generation{0};
    [[nodiscard]] bool IsValid() const { return index != ~0u; }
};
/**
 * @brief Generation-tagged reference to a bindless texture slot (mirror of @ref GeometryHandle).
 * @note CPU-only. @ref index is the shader-facing bindless slot within its pool; @ref is3D
 *       selects the 2D vs 3D bindless pool (the two have independent slot spaces);
 *       @ref generation lets @ref GPUScene::Query catch a freed/recycled slot. Pass to
 *       @ref GPUScene::Upload / Query; feed the shader uint32 fields with @ref index.
 */
struct TextureHandle
{
    uint32_t index{~0u};
    uint32_t generation{0};
    bool is3D{false};
    [[nodiscard]] bool IsValid() const { return index != ~0u; }
};

/**
 * @brief Caller-facing scene instance for @ref GPUScene::BeginScene / @ref GPUScene::EndScene.
 * @details The caller supplies a transform/material plus the bound @ref GeometryHandle;
 *          @ref EndScene resolves the GPU-owned primitive-buffer offset and geometry type
 *          from the handle, so callers never touch @ref GSInstance::resourceOffset / type.
 */
struct InstanceDesc
{
    GeometryHandle geometry{};
    float3 transform{0, 0, 0};
    quat rotation{0, 0, 0, 1};
    float3 scale{1, 1, 1};
    uint32_t materialIndex{0};
};

#pragma pack(push, 1)
struct GSMesh
{
    // Offsets are absolute, and are in Primitive buffer (bytes).
    // @ref FVertex
    uint32_t vtxOffset;
    uint32_t vtxCount;
    // LOD0 UINT32 indices
    uint32_t idxOffset;
    uint32_t idxCount;
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
struct GSInstance
{
    // TRS
    float3 transform{0, 0, 0};
    quat rotation{0, 0, 0, 1};
    float3 scale{1, 1, 1};
    uint32_t resourceOffset; // Mesh or curve set offset in Primitive buffer (bytes)
    uint32_t materialIndex; // In Material buffer (offset)
    uint32_t resourceIndex; // Debug use
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
    uint32_t type; // 0=Directional, 1=Point, 2=Spot, 3=Disk, 4=Rect, 5=Environment
    float3 color; // Normalized RGB color
    float power; // Radiant power (type-dependent unit)
    float3 position;
    float3 direction;
    // Directional
    float angularDiameter;
    // Spot/Point
    float range;
    // Spot
    float spotInnerCosAngle;
    float spotOuterCosAngle;
    // Area lights (Disk / Rect)
    float3 dpdu; // tangent u-axis (Rect: half-extent u)
    float3 dpdv; // tangent v-axis (Rect: half-extent v)
    float2 radius; // Disk radius (x, y for ellipse)
    uint32_t twoSided;
    float importance;
    float envAzimuthOffset;
};
#pragma pack(pop)
static_assert(sizeof(GSMesh) == 44);
static_assert(sizeof(GSInstance) == 56);
static_assert(sizeof(GSMaterial) == 192);
static_assert(sizeof(GSLight) == 104);

struct GPUSceneDesc
{
    uint32_t primitiveBudget = 16 * (1u << 20); // 16MB
    uint32_t curveAABBBudget = 16 * (1u << 20); // 16MB
    uint32_t instanceBudget = static_cast<uint32_t>(1e4); // # of GSInstance elements (ring)
    uint32_t materialBudget = static_cast<uint32_t>(1e3); // # of materials (ring)
    uint32_t lightBudget = static_cast<uint32_t>(1e4); // # of lights (ring)
    uint32_t texturesBudget = static_cast<uint32_t>(1e3); // # of textures
    // Max distinct geometries (meshes + curves). The residency/BLAS vectors the renderer
    // reads are reserved to this so background uploads never reallocate them; uploads beyond
    // it are rejected rather than risking a concurrent reallocation.
    uint32_t geometryBudget = static_cast<uint32_t>(1e4);
    uint32_t tlasInstanceBudget = static_cast<uint32_t>(1e4); // # of TLAS instances (ring)
    uint32_t tlasBudget = 16 * (1u << 20); // 16MB
    uint32_t tlasScratchBudget = 32 * (1u << 20); // 32MB (ring)
    // CPU-updateable dynamic geometry (deformation: skinning / sims / morphs). Quantized
    // primitive bytes (header + verts + indices, no DAG/meshlets) live in a host-coherent ring
    // sized `dynamicGeometryBudget` per frame slot, replicated across `framesInFlight + 1` slots.
    // The BLAS is a single AllowUpdate AS refit in place each frame. Default 0 = feature off (no
    // buffer allocated); examples / the editor opt in by sizing these explicitly.
    uint32_t dynamicGeometryBudget = 0; // bytes per frame slot (0 = dynamic geometry disabled)
    // The renderer's max frames in flight (its swapchain image count, @ref Renderer::GetFrameSwaps()).
    // The dynamic ring is sized to framesInFlight + 1 slots and advanced once per frame: the CPU
    // writes the next frame's slot before that frame's GPU fence, so one slot beyond the in-flight
    // count keeps the write target clear of every slot the in-flight GPU frames are still reading
    // (N+1 buffering for a non-blocking producer, mirroring the renderer's per-frame resources).
    uint32_t framesInFlight = 2;
};

/**
 * @brief Owns all GPU-resident scene data (geometry, textures, instance/material/light
 *        tables, acceleration structures) behind an asynchronous upload work queue.
 *
 * @details Typical render-build flow (after proper Renderer setup seen in Rasterizer.cpp/Pathtracer.cpp):
 *          1. `Upload(blobs, mesh/curve/texture, outHandle)` per resource — reserves
 *             memory immediately, returns @ref Result::InProgress.
 *          2. `Poll()` each frame (background drain) or `Join()` (blocking) until ready.
 *          3. `BeginScene` → fill the mapped instance/material/light spans → `EndScene`,
 *             then write the returned @ref UpdateResult offsets into the renderer UBO.
 *          4. `BuildUBO(ubo*)` to update the UBO with the latest scene data.
 *          5. [opt. ray-tracing only] `BuildTLAS(cmd)`
 *          Geometry is reclaimed explicitly via @ref Collect after destructive edits.
 *
 * @note Uploads run on a persistent background worker fed by a lock-free multi-producer
 *       queue: @ref Upload may be called from any thread at any time (including while a
 *       drain is in flight), and the owning GPUScene MAY be rendered concurrently while
 *       @ref Poll reports InProgress — geometry/textures stream into the live scene as they
 *       become @ref Result::Ready. Per-resource readiness is observable via @ref Query.
 *
 * @note The upload / residency / acceleration-structure machinery lives in @ref GPUSceneImpl,
 *       reached through @ref mImpl. Only the committed-snapshot tables and
 *       the LUT / env / primitive / TLAS handles backing the hot inline read getters remain
 *       direct members here.
 */
class GPUScene
{
public:
    /**
     * @brief Outcome of an @ref Upload / @ref Query / @ref Poll call.
     * @note `Ready` is the GPU-usable boundary, not merely "bytes copied": for geometry
     *       it means primitive/curve bytes are visible, the BLAS is built and residency
     *       is patched; for textures it means all subresources are uploaded, layouts are
     *       transitioned, and the bindless slot is shader-readable.
     */
    enum class Result
    {
        Ready,
        InProgress,
        InvalidInput,
        InvalidHandle,
        OutOfMemory,
        DecodeFailed,
        SubmitFailed,
        Cancelled, 
    };

    enum class LightSamplerType
    {
        Uniform,
        Importance
    };
    LightSamplerType mLightSamplerType = LightSamplerType::Uniform;

    [[nodiscard]] static size_t CalculateMeshPrimitiveSize(FSerializedMesh const& src);
    [[nodiscard]] static size_t CalculateCurvePrimitiveSize(FSerializedCurve const& src);
    [[nodiscard]] static size_t CalculateCurveAABBSize(FSerializedCurve const& src);

    GPUScene(RHIDevice* device, Allocator* allocator, GPUSceneDesc const& desc, AllocatorStack* frameScratch = nullptr);
    ~GPUScene();


    /**
     * @brief Reserves final resident memory for a resource and queues the rest of its
     *        upload, returning @ref Result::InProgress when accepted.
     * @details The returned handle / texture slot is valid immediately and usable as a
     *          @ref Query key while the work is pending. Hard failures that cannot
     *          succeed later (invalid input, final-residency OOM) are returned up front.
     *          Drain the queue with @ref Join (blocking) or @ref Poll (background); the
     *          resource is GPU-usable only once Query reports @ref Result::Ready.
     * @param blobs      Deserializer for the source's payloads; must outlive the drain.
     * @param source     Serialized mesh/curve/texture to upload.
     * @param outHandle  [out] Geometry handle, allocated before completion.
     * @return @ref Result::InProgress on success, else a hard error.
     */
    Result Upload(FBlobDeserializer* blobs, FSerializedMesh const& source, GeometryHandle& outHandle);
    Result Upload(FBlobDeserializer* blobs, FSerializedCurve const& source, GeometryHandle& outHandle);
    /**
     * @brief Uploads a mesh as CPU-updateable dynamic geometry (deformation workloads).
     * @details Topology (indices) is fixed; only vertex positions change per frame. The source's
     *          quantized verts + LOD0 indices are reserved in the host-coherent dynamic ring
     *          (replicated across every frame slot) and a single @ref
     * RHIAccelerationStructureBuildFlagsBits::AllowUpdate BLAS is built once. Per frame the caller rewrites the current
     * slot's verts via
     *          @ref BeginGeometryUpdate / @ref EndGeometryUpdate and the graph's
     *          "Dynamic BLAS Refit" pass (@ref RefitDynamicGeometry) refits the BLAS in place.
     *          DAG/meshlet payloads are ignored (dynamic geo is drawn with a plain vertex/index
     *          path, not the meshlet pipeline). Requires @ref GPUSceneDesc::dynamicGeometryBudget > 0.
     * @note The source's rest-pose verts seed every slot; the handle is @ref Result::Ready once
     *       its bytes are resident and the BLAS is built (synchronous, not via the upload worker).
     */
    Result UploadDynamic(FBlobDeserializer* blobs, FSerializedMesh const& source, GeometryHandle& outHandle);
    /**
     * @brief Queues a serialized texture upload, binding its bindless slot up front.
     * @param outTexture  [in,out] Pass a default (invalid) handle to allocate a new slot,
     *                    or an existing handle to update it in place.
     * @param pinned      When true the slot is a GPUScene-owned singleton that @ref Collect
     *                    must never reclaim (LUTs / defaults / env map); scene textures pass false.
     */
    Result Upload(FBlobDeserializer* blobs, FSerializedTexture const& source, TextureHandle& outTexture,
                  const char* debugName = nullptr, bool pinned = false);
    /** @brief Uploads a CPU-resident @ref FTexture through the same queue (runs to completion). */
    Result Upload(FTexture const& source, TextureHandle& outTexture, const char* debugName = nullptr,
                  bool pinned = false);
    /** @brief Queues a copy of @p data into a device-local buffer region (drained by Join/Poll). */
    Result Upload(RHIBuffer* dst, Span<const unsigned char> data, uint32_t dstOffset = 0);
    /** @brief Residency state of an uploaded geometry handle. @return Ready / InProgress / error. */
    [[nodiscard]] Result Query(GeometryHandle handle) const;
    /** @brief Residency state of an uploaded texture slot. @return Ready / InProgress / error. */
    [[nodiscard]] Result Query(TextureHandle texture) const;
    /**
     * @brief Drains all queued uploads on the calling thread, blocking until GPU-resident.
     * @note Joins an in-flight background drain (@ref Poll) first.
     */
    void Join();
    /**
     * @brief Non-blocking drain: kicks a background worker on the first call with queued work,
     *        then reports progress while streaming residency in.
     * @details The worker uploads geometry/textures and publishes residency incrementally; the
     *          owning GPUScene MAY be rendered concurrently while this returns InProgress.
     *          Instances stream into the TLAS as their geometry becomes @ref Result::Ready, and
     *          @ref Query reports per-texture residency so materials can fall back to defaults.
     * @return @ref Result::InProgress while draining, @ref Result::Ready when complete (or when
     *         nothing is queued), or an error Result on failure.
     */
    [[nodiscard]] Result Poll();

    /**
     * @brief Uploads an environment map and computes its importance-sampling CDFs.
     * @return @ref Result::Ready on success.
     */
    Result UploadEnvMap(FTexture const& source);

    /**
     * @brief Writes every GPUScene-owned global bindless index into the renderer UBO:
     *        the GGX/sheen LUTs and the environment map + its importance-sampling CDFs
     *        (default-substituted when no env map is loaded).
     * @note Also copies the latest instance/material/light table offsets committed by @ref EndScene.
     */
    void BuildUBO(RendererUBO& globals) const;

    /**
     * @brief Ring-buffer offsets and element counts for instances/materials/lights.
     */
    struct UpdateResult
    {
        uint32_t firstInstance{0u};
        uint32_t numInstances{0u};
        uint32_t firstMaterial{0u};
        uint32_t numMaterials{0u};
        uint32_t firstLight{0u};
        uint32_t firstLightAliasTable{0u};
        uint32_t numLights{0u};
        float sceneLightImportanceSum{0.0f};
    };

    /**
     * @brief GPUScene-owned, caller-filled spans for the instance/material/light tables,
     *        plus their ring-buffer base offsets.
     * @note `instances` is a caller-facing @ref InstanceDesc scratch span (not GPU memory);
     *       @ref EndScene translates it into the GSInstance ring. `materials`/`lights` are
     *       mapped GPU ring memory filled in place.
     */
    struct GPUSceneTables
    {
        Span<InstanceDesc> instances;
        Span<GSMaterial> materials;
        Span<GSLight> lights;
        uint32_t firstInstance{0};
        uint32_t firstMaterial{0};
        uint32_t firstLight{0};
        uint32_t firstLightAliasTable{0};
    };
    /**
     * @brief Begins a scene-table update, returning caller-fill spans.
     * @details Fill `instances` with @ref InstanceDesc (geometry handle + transform +
     *          material); @ref EndScene resolves the GPU-owned geometry offset/type. The
     *          `materials`/`lights` spans are mapped GPU memory filled in place. Must be
     *          paired with @ref EndScene.
     */
    GPUSceneTables BeginScene(uint32_t instanceCount, uint32_t materialCount, uint32_t lightCount);
    /**
     * @brief Commits the filled spans: patches instance geometry fields, snapshots the
     *        tables for TLAS/picking/@ref Collect, and computes the light alias table.
     * @return Ring-buffer offsets/counts to populate the UBO with.
     */
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
    /**
     * @brief Builds (update=false) or refits (update=true) the TLAS from the committed
     *        instance table; instances whose geometry isn't yet @ref Result::Ready are skipped.
     * @note The TLAS lives in buffers pre-allocated to @ref GPUSceneDesc::tlasBudget and is
     *       never reallocated: the AS device object stays stable, so the render graph's captured
     *       handle never goes stale and no renderer rebuild is needed. A committed instance set
     *       whose TLAS would exceed the budget is a hard error (raise tlasBudget).
     */
    [[nodiscard]] TLASBuildResult BuildTLAS(RHICommandList* cmd, bool update = false);

    /* --- Dynamic (CPU-updateable) geometry: per-frame deformation ---
     * Per-frame contract (mirrors a renderer Begin/End frame bracket):
     *     BeginDynamicGeometryUpdate();              // once per frame, advances the ring slot
     *     for each deforming geo: fill UpdateDynamicGeometry(handle);
     *     EndDynamicGeometryUpdate();                // closes the window
     *     ... BeginScene/EndScene, then the graph's "Dynamic BLAS Refit" pass ...
     * @ref UpdateDynamicGeometry may only be called inside the open window; the window state also
     * guards the per-frame getters. */
    /** @brief True when at least one dynamic geometry is resident. */
    [[nodiscard]] bool HasDynamicGeometry() const;
    /**
     * @brief Opens the per-frame dynamic-geometry update window and advances the ring to the next
     *        frame slot. Call exactly once per rendered frame before any @ref UpdateDynamicGeometry.
     * @details The slot the CPU writes (current) is distinct from the slot the GPU traced last
     *          frame, mirroring the table rings' frames-in-flight invariant. Must be paired with
     *          @ref EndDynamicGeometryUpdate.
     */
    void BeginDynamicGeometryUpdate();
    /**
     * @brief Returns the mapped vertex sub-span (quantized @ref FQVertex bytes) for @p handle in
     *        the *current* frame slot and marks it dirty for this frame's BLAS refit. Lock-free:
     *        each dynamic handle owns a disjoint region. Only valid inside the
     *        @ref BeginDynamicGeometryUpdate / @ref EndDynamicGeometryUpdate window.
     */
    [[nodiscard]] Span<std::byte> UpdateDynamicGeometry(GeometryHandle handle);
    /** @brief Closes the per-frame dynamic-geometry update window opened by @ref BeginDynamicGeometryUpdate. */
    void EndDynamicGeometryUpdate();
    /**
     * @brief Refits (or periodically rebuilds) every dirty dynamic geometry's BLAS in place
     *        against the current frame slot. Record in a render-graph pass that runs *before*
     *        the "TLAS Update" pass, declaring an acceleration-structure write on the BLAS region.
     */
    void BuildBLAS(RHICommandList* cmd);
    /** @brief Frames between forced full BLAS rebuilds for dynamic geo (0 = refit only). Default 64. */
    void SetDynamicGeometryRebuildRate(uint32_t framesOrZero);
    /** @brief Dynamic geos refitted in the last @ref RefitDynamicGeometry call. */
    [[nodiscard]] uint32_t GetDynamicRefitCount() const;
    /** @brief Of the last refit, how many were full rebuilds (the rest were in-place updates). */
    [[nodiscard]] uint32_t GetDynamicRebuildCount() const;

    /**
     * @brief Garbage-collects geometry and textures no longer referenced by the committed
     *        scene tables.
     * @note Marks geometry referenced by the committed instance table and (non-pinned)
     *       textures referenced by the committed material table, then frees the rest
     *       (geometry + their BLAS; texture bindless slots, bumping their generation so
     *       stale @ref TextureHandle values fail @ref Query). Pinned textures (LUTs /
     *       defaults / env map) and in-flight uploads are kept. The caller must ensure the
     *       GPU is no longer using the reclaimed resources (e.g. WaitIdle after a
     *       destructive edit).
     */
    void Collect();

    /* --- Committed scene reads (snapshots; index == TLAS instanceID for instances) --- */
    [[nodiscard]] GSInstance GetInstance(uint32_t index) const
    {
        CHECK_MSG(index < mCommittedInstances.size(), "GetInstance index {} out of range ({})", index,
                  mCommittedInstances.size());
        return mCommittedInstances[index];
    }
    [[nodiscard]] uint32_t GetInstanceCount() const { return static_cast<uint32_t>(mCommittedInstances.size()); }
    /** @brief Maps a TLAS pick id (instanceID) to its committed instance index, or UINT32_MAX. */
    [[nodiscard]] uint32_t ResolvePickedInstance(uint32_t pickID) const
    {
        // pickID is a TLAS instanceID = index into the last build's written (ready) set.
        if (pickID >= mPickMap.size())
            return UINT32_MAX;
        return mPickMap[pickID];
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
    /**
     * @brief The host-coherent dynamic (CPU-updateable) primitive ring, bound as a second
     *        primitive storage buffer; instances with @ref kGSInstanceFlagDynamic read from it.
     * @return The dynamic ring, or the immutable primitive buffer as a fallback when the feature
     *         is disabled (so the binding is always valid; no dynamic instance ever selects it).
     */
    [[nodiscard]] RHIBuffer* GetDynamicPrimitiveBuffer() const;
    [[nodiscard]] RHIBuffer* GetInstanceBuffer() const;
    [[nodiscard]] RHIBuffer* GetMaterialBuffer() const;
    [[nodiscard]] RHIBuffer* GetLightBuffer() const;
    [[nodiscard]] RHIBuffer* GetLightAliasTableBuffer() const;
    /* Textures */
    [[nodiscard]] BindlessPool* GetTexture2DPool();
    [[nodiscard]] BindlessPool* GetTexture3DPool();
    [[nodiscard]] BindlessPool const* GetTexture2DPool() const;
    [[nodiscard]] BindlessPool const* GetTexture3DPool() const;
    [[nodiscard]] uint32_t GetGGXLutEIndex() const { return mLUTGGXEIndex.index; }
    [[nodiscard]] uint32_t GetGGXLutEavgIndex() const { return mLUTGGXEavgIndex.index; }
    [[nodiscard]] uint32_t GetGGXLutEIORIndex() const { return mLUTGGXEIORIndex.index; }
    [[nodiscard]] uint32_t GetGGXLutEIORavgIndex() const { return mLUTGGXEIORavgIndex.index; }
    [[nodiscard]] uint32_t GetGGXLutEIORInvIndex() const { return mLUTGGXEIORInvIndex.index; }
    [[nodiscard]] uint32_t GetGGXLutEIORInvavgIndex() const { return mLUTGGXEIORInvavgIndex.index; }
    [[nodiscard]] uint32_t GetSheenLtcIndex() const { return mLUTSheenLTCIndex.index; }
    [[nodiscard]] RHITexture* GetFoundationDefaultTexture2D() const;
    [[nodiscard]] RHITexture* GetFoundationDefaultTexture2DFloat() const;
    [[nodiscard]] RHIBuffer* GetFoundationDefaultBufferFloat() const { return mFoundationDefaultBufferFloat.Get(); }
    // Environment map
    [[nodiscard]] bool HasEnvMap() const { return mEnvMapIndex.IsValid(); }
    [[nodiscard]] uint32_t GetEnvMapIndexOrDefault() const
    {
        return HasEnvMap() ? mEnvMapIndex.index : mFoundationDefaultTexture2DIndex.index;
    }
    [[nodiscard]] uint32_t GetEnvMapMarginalCDFIndexOrDefault() const
    {
        return mEnvMapMarginalCDFIndex.IsValid() ? mEnvMapMarginalCDFIndex.index
                                                 : mFoundationDefaultTexture2DFloatIndex.index;
    }
    [[nodiscard]] uint32_t GetEnvMapConditionalCDFIndexOrDefault() const
    {
        return mEnvMapConditionalCDFIndex.IsValid() ? mEnvMapConditionalCDFIndex.index
                                                    : mFoundationDefaultTexture2DFloatIndex.index;
    }
    /* AS */
    [[nodiscard]] RHIAccelerationStructure* GetTLAS() const
    {
        // Returns the allocated TLAS whenever the scene has one, independent of how many
        // instances are currently resident. This lets ray passes be built into the render
        // graph up front; BuildTLAS keeps it valid (an empty 0-instance TLAS) while geometry
        // streams in, so instances appear without a renderer rebuild.
        return mTLAS.IsValid() ? mTLAS.Get() : nullptr;
    }
    /* Samplers */
    [[nodiscard]] RHIBuffer* GetSobolMatricesBuffer() const { return mSobolMatricesBuffer.Get(); }

    void Reset();

private:
    friend struct GPUSceneImpl;

    // Committed table snapshots (filled via BeginScene/EndScene). These are the
    // authoritative CPU-side scene used by BuildTLAS, picking, and Collect(); they do
    // not depend on ring memory that may be overwritten by a later commit.
    Vector<GSInstance> mCommittedInstances;
    Vector<GSLight> mCommittedLights;
    Vector<GSMaterial> mCommittedMaterials;
    UpdateResult mLastUpdateResult;
    // TLAS instanceID -> committed instance index, rebuilt each BuildTLAS. When some
    // referenced geometry is not yet Ready those instances are skipped, so the TLAS id
    // is no longer identical to the committed index and picking must go through this map.
    Vector<uint32_t> mPickMap;

    // Plain handles backing the hot inline getters above; created/managed by GPUSceneImpl.
    RHIDeviceScopedHandle<RHIBuffer> mPrimitiveBuffer;
    RHIDeviceScopedHandle<RHIAccelerationStructure> mTLAS;
    uint32_t mLastTLASInstancesCount{0};
    RHIDeviceScopedHandle<RHIBuffer> mSobolMatricesBuffer;
    RHIDeviceScopedHandle<RHIBuffer> mFoundationDefaultBufferFloat;
    // Precomputed LUTs (texture2D pool)
    TextureHandle mLUTGGXEIndex;
    TextureHandle mLUTGGXEavgIndex;
    TextureHandle mLUTGGXEIORavgIndex;
    TextureHandle mLUTGGXEIORInvavgIndex;
    // Precomputed LUTs (texture3D pool)
    TextureHandle mLUTGGXEIORIndex;
    TextureHandle mLUTGGXEIORInvIndex;
    TextureHandle mLUTSheenLTCIndex;
    // Shared default resources (texture2D pool)
    TextureHandle mFoundationDefaultTexture2DIndex;
    TextureHandle mFoundationDefaultTexture2DFloatIndex;
    // Environment map (texture2D pool)
    TextureHandle mEnvMapIndex;
    TextureHandle mEnvMapMarginalCDFIndex;
    TextureHandle mEnvMapConditionalCDFIndex;

    UniquePtr<GPUSceneImpl> mImpl;
};

ENUM_NAME_CONV_BEGIN(GPUScene::Result)
ENUM_NAME(Ready)
ENUM_NAME(InProgress)
ENUM_NAME(InvalidInput)
ENUM_NAME(InvalidHandle)
ENUM_NAME(OutOfMemory)
ENUM_NAME(DecodeFailed)
ENUM_NAME(SubmitFailed)
ENUM_NAME(Cancelled)
ENUM_NAME_CONV_END()
