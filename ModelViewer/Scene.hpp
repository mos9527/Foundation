#pragma once
#include <Core/Pool.hpp>
#include <Rendering/GPUScene.hpp>
#include "Assets/Mesh.hpp"
namespace ModelViewer {
    using namespace Foundation;
    using namespace Rendering;
    using SceneHandle = uint32_t;
    constexpr uint32_t kSceneInvalid = ~0u;
    constexpr size_t kMaxSceneMeshLodCount = 4;
    /* -- Mesh -- */
    struct SceneMeshLodData
    {
        Vector<MeshIndex> indices;
        Vector<MeshMeshlet> meshlets;
        Vector<MeshIndex> meshletVertices;
        Vector<MeshMicroIndex> meshletTriangles;
        // TODO: [De]serialization? Meshlet generation can be expensive.
        SceneMeshLodData(Allocator* allocator) : indices(allocator), meshlets(allocator), meshletVertices(allocator), meshletTriangles(allocator) {}
    };
    struct SceneMeshData
    {
        Vector<MeshVertex> vertices;
        Vector<SceneMeshLodData> lods;
        SceneMeshData(Allocator* allocator) : vertices(allocator), lods(allocator) {}
    };
    SceneMeshData sceneMeshFromVertexIndex(Span<MeshVertex> vertices, Span<MeshIndex> indices, Allocator* allocator, int numLods = kMaxSceneMeshLodCount, bool buildMeshlets = true);

    struct SceneMeshLodAllocation
    {
        VirtualAllocation indices;
        VirtualAllocation meshlets;
        VirtualAllocation meshletVertices;
        VirtualAllocation meshletTriangles;
        size_t indexCount = 0;
        size_t meshletCount = 0;
    };
    struct SceneMeshAllocation
    {
        VirtualAllocation vertices;
        Array<SceneMeshLodAllocation, kMaxSceneMeshLodCount> lods;
        size_t lodCount = 0;
    };
    class Scene
    {
        Allocator* mAllocator;

        GPUScene& mGPUScene;
        Pool<SceneHandle, SceneMeshAllocation> mMeshes;
    public:
        Scene(GPUScene& scene, Allocator* allocator);
        SceneHandle PushMesh(SceneMeshData const& data);
        void FreeMesh(SceneHandle handle);
    };
}