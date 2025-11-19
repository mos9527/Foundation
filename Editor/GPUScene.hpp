#pragma once
#include "Context.hpp"
#include <RenderCore/Streaming.hpp>
#include <Math/Math.hpp>
using namespace Math;

BITMASK_ENUM_BEGIN(GSData, uint8_t)
    Mesh = 1 << 0,
BITMASK_ENUM_END()

static const size_t kNumMeshDiscreteLODs = 5;

struct GSMeshLOD
{
    uint32_t indOffset;
    uint32_t indCount;
    uint32_t meshletOffset;
    uint32_t meshletCount;
    uint32_t meshletVtxOffset;
    uint32_t meshletTriOffset;
};
struct GSMesh
{
    uint32_t vtxOffset;
    uint32_t vtxCount;
    uint32_t lodCount;
    GSMeshLOD lod[kNumMeshDiscreteLODs];
};

struct GSInstance
{
    // TRS
    float3 transform;
    quat rotation;
    float3 scale;
    // Bitfield LE [8 GSData] [24 Offset]
    uint32_t data;
};

/**
* @brief Async GPU scene data storage for Editor.
*/
class GPUScene
{
    FEditorContext* mContext;

    StreamingPool mStreaming;

    // Staged from StreamingPool
    RHIDeviceUniqueRef<RHIBuffer> mPrimitiveBuffer;
    // XXX: Linear allocation. GPA would be needed if we'd upload & free
    //      at will. Not needed for Editor use-case currently.
    AllocatorStack mPrimitiveAlloc;

    // Mapped as-is
    RHIDeviceUniqueRef<RHIBuffer> mInstanceBuffer;
    Span<char> mInstanceData;

    List<SharedFuture<>> mTasksMeshUpload;
public:
    struct GPUSceneDesc
    {
        size_t primitiveBudget = 16 * (1u<<20); // 16MB
        size_t instanceBudget = 4 * (1u<<20);  // 4MB
    };
    GPUScene(FEditorContext* ctx, GPUSceneDesc const& desc);

    struct UploadMeshData
    {
        Span<const char> vtx;
        uint32_t vtxCount;
        struct LOD
        {
            Span<const char> ind;
            uint32_t indCount;
            Span<char> meshletData;
            uint32_t meshletCount;
            Span<char> meshletVtx; // uint8_t indices
            Span<char> meshletTri; // uint8_t indices
        };
    };
    SharedFuture<> UploadMeshAsync(uint32_t& outOffset, GSMesh& outData, UploadMeshData const& source);

    PassHandle CreatePass(Renderer* r);
};