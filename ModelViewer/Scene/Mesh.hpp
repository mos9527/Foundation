#pragma once
#include <Math/Math.hpp>
#include <Core/Core.hpp>
#include <Assets/Mesh.hpp>
#include <Rendering/VirtualAllocator.hpp>
namespace ModelViewer
{
    using namespace Foundation::Core;
    using namespace Foundation::Math;
    using namespace Foundation::Rendering;
    constexpr int kMaxSceneMeshLodCount = 4;
    // Intermediate scratch buffers
    // TODO: We may want to actually cache these. Meshlet generation can be expensive.
    struct MeshLodScratchBuffers
    {
        Vector<MeshIndex> indices;
        Vector<MeshMeshlet> meshlets;
        Vector<MeshIndex> meshletVertices;
        Vector<MeshMicroIndex> meshletTriangles;
        MeshLodScratchBuffers(Allocator* allocator) : indices(allocator), meshlets(allocator), meshletVertices(allocator), meshletTriangles(allocator) {}
    };
    struct MeshScratchBuffers
    {
        Vector<MeshVertexCompact> vertices;
        Vector<MeshLodScratchBuffers> lods;
        MeshScratchBuffers(Allocator* allocator) : vertices(allocator), lods(allocator) {}
    };
    MeshScratchBuffers sceneMeshDataFromVertexIndex(Span<MeshVertex> vertices, Span<MeshIndex> indices, Allocator* allocator, int numLods = kMaxSceneMeshLodCount, bool buildMeshlets = true);
#pragma pack(push, 4)
    struct MeshLodAllocation
    {
        VirtualAllocation indices{kInvalidVirtualAllocation};
        VirtualAllocation meshlets{kInvalidVirtualAllocation};
        VirtualAllocation meshletVertices{kInvalidVirtualAllocation};
        VirtualAllocation meshletTriangles{kInvalidVirtualAllocation};
        uint32_t indexCount;
        uint32_t meshletCount;

        uint32_t indexRawOffset;
        uint32_t meshletRawOffset;
        uint32_t meshletVerticesRawOffset;
        uint32_t meshletTrianglesRawOffset;
    };
    /**
     * 4-byte aligned. Allocated in the Const buffer in the @ref GPUScene
     * @ref VirtualAllocation fields are unused in the shaders.
     */
    struct MeshAllocation
    {
        VirtualAllocation vertices{kInvalidVirtualAllocation};
        uint32_t vertexCount;
        uint32_t vertexRawOffset;
        uint32_t lodCount;
        MeshLodAllocation lods[kMaxSceneMeshLodCount];
        // We store ourselves in the Const buffer as well
        VirtualAllocation self{kInvalidVirtualAllocation};
        uint32_t selfRawOffset;
    };
#pragma pack(pop)
}
