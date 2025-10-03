#include "Scene.hpp"
namespace ModelViewer
{
    SceneMeshData sceneMeshDataFromVertexIndex(Span<MeshVertex> vertices, Span<MeshIndex> indices, Allocator* allocator,
                                           int numLods, const bool buildMeshlets)
    {
        CHECK_MSG(numLods >= 1 && numLods <= kMaxSceneMeshLodCount, "numLods ({}) out of range [1, {}]", numLods, kMaxSceneMeshLodCount);
        SceneMeshData res(allocator);
        auto packed = Views::all(vertices) | Views::transform(
                                   [](auto const& v) { return MeshVertexCompact::Pack(v); });
        res.vertices.insert(res.vertices.end(), packed.begin(), packed.end());
        for (int lod = 0; lod < numLods; ++lod)
        {
            auto& mesh = res.lods.emplace_back(allocator);
            if (lod == 0)
                mesh.indices.insert(mesh.indices.end(), indices.begin(), indices.end());
            else
            {
                float lodScale = 1.f - static_cast<float>(lod) / static_cast<float>(numLods - 1);
                meshGenerateLod(mesh.indices, vertices, res.lods[lod - 1].indices, lodScale);
            }
            if (buildMeshlets)
                meshBuildMeshlets(mesh.meshlets, mesh.meshletVertices, mesh.meshletTriangles,
                                  vertices, mesh.indices);
        }
        return res;
    }
    Scene::Scene(GPUScene* scene, Allocator* allocator) : mAllocator(allocator), mGPUScene(scene), mMeshes(allocator) {}
    ScenePushConstants Scene::GetSceneConstants() { 
        ScenePushConstants pc{};
        mat4 proj = infinitePerspective(mCamera.verticalFov, mCamera.aspectRatio, mCamera.zNear);
        mat4 view = lookAt(mCamera.position, mCamera.lookAt, float3{0, 0, 1});
        proj[1][1] *= -1; // Vulkan NDC
        pc.viewProj = proj * view;
        return pc;
    }
    SceneHandle Scene::PushMesh(SceneMeshData const& data)
    {
        auto [handle, alloc] = mMeshes.PopPair();
        alloc.vertices = mGPUScene->PushConst(
            Span<const MeshVertexCompact>(data.vertices).AsBytes(), alignof(MeshVertexCompact)
        );
        alloc.vertexCount = data.vertices.size();
        alloc.vertexRawOffset = mGPUScene->QueryConst(alloc.vertices).first;
        alloc.lodCount = data.lods.size();
        for (size_t i = 0; i < alloc.lodCount; ++i)
        {
            auto& src = data.lods[i];
            auto& dst = alloc.lods[i];
            dst.indices = mGPUScene->PushConst(
                Span<const MeshIndex>(src.indices).AsBytes(), alignof(MeshIndex)
            );
            dst.indexCount = src.indices.size();
            dst.indexRawOffset = mGPUScene->QueryConst(dst.indices).first;
            if (!src.meshlets.empty())
            {
                dst.meshlets = mGPUScene->PushConst(
                    Span<const MeshMeshlet>(src.meshlets).AsBytes(), alignof(MeshMeshlet)
                );
                dst.meshletCount = src.meshlets.size();
                dst.meshletRawOffset = mGPUScene->QueryConst(dst.meshlets).first;
                dst.meshletTriangles = mGPUScene->PushConst(
                    Span<const MeshMicroIndex>(src.meshletTriangles).AsBytes(), alignof(MeshMicroIndex)
                );
                dst.meshletTrianglesRawOffset = mGPUScene->QueryConst(dst.meshletTriangles).first;
                dst.meshletVertices = mGPUScene->PushConst(
                    Span<const MeshIndex>(src.meshletVertices).AsBytes(), alignof(MeshIndex)
                );
                dst.meshletVerticesRawOffset = mGPUScene->QueryConst(dst.meshletVertices).first;
            }
        }
        alloc.self = mGPUScene->PushConst(
            Span<const SceneMeshAllocation>(alloc).AsBytes(), alignof(SceneMeshAllocation)
        );
        alloc.selfRawOffset = mGPUScene->QueryConst(alloc.self).first;
        return handle;
    }
    SceneMeshAllocation const& Scene::QueryMesh(SceneHandle handle)
    {
        return mMeshes.At(handle);
    }
    void Scene::FreeMesh(SceneHandle handle)
    {
        auto const& alloc = mMeshes.At(handle);
        mGPUScene->FreeConst(alloc.vertices);
        for (size_t i = 0; i < alloc.lodCount; ++i)
        {
            auto const& lod = alloc.lods[i];
            mGPUScene->FreeConst(lod.indices);
            if (lod.meshlets != kInvalidVirtualAllocation)
            {
                mGPUScene->FreeConst(lod.meshlets);
                mGPUScene->FreeConst(lod.meshletTriangles);
                mGPUScene->FreeConst(lod.meshletVertices);
            }
        }
        mGPUScene->FreeConst(alloc.self);
        mMeshes.Free(handle);
    }
    Span<SceneInstanceData> Scene::MapInstances() { 
        return mGPUScene->MapInstanceData<SceneInstanceData>();
    }
    void Scene::UnmapInstances() { 
        mGPUScene->UnmapInstanceData(); 
    }
} // namespace ModelViewer
