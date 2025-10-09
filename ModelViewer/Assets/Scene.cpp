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
    Scene::Scene(GPUScene* scene, Allocator* allocator) : mAllocator(allocator), mGPUScene(scene), mMeshes(allocator)
    {
        mSceneParamsAllocation = scene->PushShared(Span<Params>(mParams).AsBytes(), alignof(Params));
    }
    SceneCamera::Params SceneCamera::GetParams() const
    { 
        mat4 proj = infinitePerspective(verticalFov, aspectRatio, zNear);
        mat4 view = lookAtRH(position, lookAt, up);
        proj[1][1] *= -1; // Vulkan NDC
        return {.viewProj = proj * view, .cameraPosition = position, .zNear = zNear};
    }
    SceneCamera::CullParams SceneCamera::GetCullParams() const
    {
        mat4 proj = infinitePerspective(verticalFov, aspectRatio, zNear);
        mat4 view = lookAtRH(position, lookAt, up);
        view[1][1] *= -1; // Vulkan NDC
        // See also
        // - Fast Extraction of Viewing Frustum Planes from the WorldView-Projection Matrix
        // - https://github.com/zeux/niagara/blob/master/src/niagara.cpp
        mat4 projT = transpose(proj);
        // vvv Plane equations forming ax+by+cz+d=0
        float4 pLeft = projT[3] + projT[0];   // (m41 + m11, m42 + m12, m43 + m13, m44 + m14)
        float4 pTop = projT[3] + projT[1];    // (m41 + m21, m42 + m22, m43 + m23, m44 + m24)
        // ^^^
        // Note that this would be in View space (not incl. view matrix) - so origin is always on the plane
        // w=0 -> ax+by+cz=0
        // It's also easy to notice that for left/right planes y=0, for top/bottom planes x=0:
        // pLeft -> ax+cz = 0, pTop -> by+cz=0
        // Since our projection matrix is symmetric (l=-r, t=-b), so pRight, pBottom would simply be:
        // pRight -> -ax+cz=0, pBottom -> -by+cz=0
        // 4 floats would be enough for 4 planes.
        // For near and far plane - just use the view space depth.
        // vvv Normalize
        pLeft /= length(pLeft.xyz()), pTop /= length(pTop.xyz());
        return {.viewMatrix = view, .frustum = {
         pLeft.x, pLeft.z, pTop.y , pTop.z,
        }};
    }
    SceneGrid::Params SceneGrid::GetParams(SceneCamera const& camera) const
    {
        return {
            .camera = camera.GetParams(), 
            .dimension = dimension, .width = width, 
            .type = static_cast<uint>(type)
        };
    }
    void Scene::CommitParams()
    {
        mParams.camera = mCamera.GetParams();
        mParams.cullParams = mCullingCamera.GetCullParams();
        mParams.instanceCount = mInstanceCount; // TODO. Again...
        mGPUScene->UpdateShared(mSceneParamsAllocation,Span<Params>(mParams).AsBytes());
    }
    VirtualAllocation Scene::GetParamsAllocationRawOffset() const
    {
        return mGPUScene->QueryShared(mSceneParamsAllocation).first;
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
