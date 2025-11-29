#pragma once
#include "Context.hpp"
#include "Mesh.hpp"
#include <RenderCore/Streaming.hpp>
#include <Math/Math.hpp>
using namespace Math;

BITMASK_ENUM_BEGIN(GSData, uint8_t)
    Mesh = 1 << 0,
BITMASK_ENUM_END()

#pragma pack(push, 1)
struct GSMesh
{
    // Offsets are absolute, and are in Primitive buffer (bytes).
    // @ref FVertex
    uint32_t vtxOffset;
    uint32_t vtxCount;
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
    float3 transform{0,0,0};
    quat rotation{0,0,0,1};
    float3 scale{1,1,1};
    uint32_t meshOffset; // In Primitive buffer (bytes)
};
#pragma pack(pop)
static_assert(sizeof(GSMesh) == 36);
static_assert(sizeof(GSInstance) == 44);

/**
* @brief Async GPU scene data storage for Editor.
*/
class GPUScene
{
    FContext* mContext;

    StreamingPool mStreaming;
    RHIDeviceScopedHandle<RHIBuffer> mPrimitiveBuffer;
    // XXX: Linear allocation. GPA would be needed if we'd upload & free
    //      at will. Not needed for Editor use-case currently.
    size_t mPrimitiveOffset{0};

    // For @ref meshletGlobalIndex
    uint32_t mMeshletGlobalCounter{0};

    // Mapped as-is
    RHIDeviceScopedHandle<RHIBuffer> mInstanceBuffer;
    GSInstance *mInstanceBegin, *mInstanceRing, *mInstanceEnd;
public:
    struct GPUSceneDesc
    {
        size_t primitiveBudget = 16 * (1u<<20); // 16MB
        size_t instanceBudget = (size_t)1e3; // # of instances
    };
    GPUScene(FContext* ctx, GPUSceneDesc const& desc);

    /**
     * @breif Allocates `count` instances in the Instance ring buffer, returning a Span to the allocated memory.
     *        It's up to the caller to fill in the data, which is usually write-only.
     * @note There's no guard against allocating potentially still in-flight memory range.
     *       Ensure enough @ref GPUSceneDesc::instanceBudget to avoid overwriting.
     * @return Raw mapped memory ptr, offset (element wise) in buffer.
     */
    Pair<GSInstance*, uint32_t> InstanceAlloc(uint32_t count);

    /**
     * @brief Uploads mesh data asynchronously to GPU, returning a future that completes when upload is done.
     *        outData is immediately filled with offsets/counts, but the actual GPU data will only be valid once the
     * future is completed.
     * @note  source is copied internally, and thus is not required to be kept alive.
     */
    Future<> Upload(FMesh const& source, GSMesh& outData, uint32_t& outOffset);

    String DbgGetStatistics() const;

    RHIBuffer* GetPrimitiveBuffer() const { return mPrimitiveBuffer.Get(); }
    RHIBuffer* GetInstanceBuffer() const { return mInstanceBuffer.Get(); }

    void Reset();
};