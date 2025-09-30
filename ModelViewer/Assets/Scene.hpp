#pragma once
#include <Core/Pool.hpp>
#include <Rendering/GPUScene.hpp>
#include "Mesh.hpp"
namespace ModelViewer {
    using namespace Foundation;
    using namespace Rendering;
    using SceneHandle = uint32_t;
    constexpr uint32_t kSceneInvalid = ~0u;
    constexpr size_t kMaxSceneMeshLodCount = 4;
    /* -- Mesh -- */
    // Intermediate scratch buffers
    // TODO: We may want to actually cache these. Meshlet generation can be expensive.
    struct SceneMeshLodData
    {
        Vector<MeshIndex> indices;
        Vector<MeshMeshlet> meshlets;
        Vector<MeshIndex> meshletVertices;
        Vector<MeshMicroIndex> meshletTriangles;
        SceneMeshLodData(Allocator* allocator) : indices(allocator), meshlets(allocator), meshletVertices(allocator), meshletTriangles(allocator) {}
    };
    struct SceneMeshData
    {
        Vector<MeshVertexCompact> vertices;
        Vector<SceneMeshLodData> lods;
        SceneMeshData(Allocator* allocator) : vertices(allocator), lods(allocator) {}
    };
    SceneMeshData sceneMeshFromVertexIndex(Span<MeshVertex> vertices, Span<MeshIndex> indices, Allocator* allocator, int numLods = kMaxSceneMeshLodCount, bool buildMeshlets = true);
    struct SceneMeshLodAllocation
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
    struct SceneMeshAllocation
    {
        VirtualAllocation vertices{kInvalidVirtualAllocation};
        Array<SceneMeshLodAllocation, kMaxSceneMeshLodCount> lods;
        uint32_t vertexCount;
        uint32_t vertexRawOffset;
        uint32_t lodCount;
        // We store ourselves in the Const buffer as well
        VirtualAllocation self{kInvalidVirtualAllocation};
        uint32_t selfRawOffset;
    };
    /* -- Instance -- */
    /**
     * 4-byte aligned. Allocated in the Instance buffer in the @ref GPUScene
     */
    struct SceneInstanceData
    {
        vec3 t; // Translation
        vec4 q; // Rotation Quat (xyzw)
        vec3 s; // Scale
        // @ref SceneMeshAllocation::selfRawOffset + 1
        // 0 reserved for no mesh
        uint32_t meshAllocationRawOffsetPP = 0;
    };
    class Scene
    {
        Allocator* mAllocator;

        GPUScene& mGPUScene;
        Pool<SceneHandle, SceneMeshAllocation> mMeshes;
    public:
        Scene(GPUScene& scene, Allocator* allocator);

        SceneHandle PushMesh(SceneMeshData const& data);
        SceneMeshAllocation const& QueryMesh(SceneHandle handle);
        void FreeMesh(SceneHandle handle);
    };
}