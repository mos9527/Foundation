#include "Scene.hpp"
namespace ModelViewer
{

    SceneMeshData sceneMeshFromVertexIndex(Span<MeshVertex> vertices, Span<MeshIndex> indices, Allocator* allocator,
                                           int numLods, bool buildMeshlets)
    {
        CHECK_MSG(numLods >= 1 && numLods <= kMaxSceneMeshLodCount, "numLods ({}) out of range [1, {}]", numLods, kMaxSceneMeshLodCount);
        SceneMeshData res(allocator);
        res.vertices.insert(res.vertices.end(), vertices.begin(), vertices.end());
        for (int lod = 0; lod < numLods; ++lod)
        {
            auto& mesh = res.lods.emplace_back(allocator);
            if (lod == 0)
                mesh.indices.insert(mesh.indices.end(), indices.begin(), indices.end());
            else
            {
                meshGenerateLod(mesh.indices, vertices, res.lods[lod - 1].indices, 1.f - float(lod) / float(numLods - 1), allocator);
            }
            if (buildMeshlets)
                meshBuildMeshlets(mesh.meshlets, mesh.meshletVertices, mesh.meshletTriangles,
                                  res.vertices, mesh.indices);
        }
    }
    Scene::Scene(GPUScene& scene, Allocator* allocator) : mGPUScene(scene), mAllocator(allocator), mMeshes(allocator) {}
} // namespace ModelViewer
