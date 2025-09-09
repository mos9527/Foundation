#include "Scene.hpp"
namespace Foundation {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    using namespace Foundation::Rendering;
    Scene::Scene(Allocator* allocator, RHIDevice* device, SceneDataDesc const& desc) :
        m_allocator(allocator), m_data(allocator, device, desc) {}

    SceneHandle Scene::AddMesh(StlSpan<const Vertex> vertices, StlSpan<const Index> indices, SceneHandle& outVtx, SceneHandle& outIdx) {
        SceneHandle vtx = m_data.AddVertexData(vertices.AsBytes());
        SceneHandle idx = m_data.AddIndexData(indices.AsBytes());
        PrimitiveMetadata prim{
            .vertexOffset = static_cast<int>(m_data.QueryVertexDataSizeAndOffset(vtx).second),
            .indexCount = static_cast<int>(indices.size()),
            .indexOffset = static_cast<int>(m_data.QueryIndexDataSizeAndOffset(idx).second),
            .sphereBounds = {} // !! TODO
        };
        outVtx = vtx, outIdx = idx;
        return m_data.AddPrimitiveData(StlSpan<PrimitiveMetadata>{prim}.AsBytes());
    };
    void Scene::FreeMesh(SceneHandle mesh, SceneHandle vtx, SceneHandle idx) {
        // TODO: Ref-counting usages?
        m_data.FreePrimitiveData(mesh);
        m_data.FreeVertexData(vtx);
        m_data.FreeIndexData(idx);
    }
    SceneHandle Scene::AddInstance(InstanceMetadata data)
    {
        m_dirty = true;
        SceneHandle hdl = m_data.AddInstanceData(StlSpan<InstanceMetadata>{data}.AsBytes());
        return hdl;
    }
    void Scene::UpdateInstance(SceneHandle instance, InstanceMetadata const& data)
    {
        m_dirty = true;
        m_data.UpdateInstanceData(instance, StlSpan<const InstanceMetadata>{data}.AsBytes());
    }
    void Scene::FreeInstance(SceneHandle handle)
    {
        m_dirty = true;
        const InstanceMetadata tombstone = {
            .enabled = 0,
            .primitiveID = ~0u,
            .transform = {}
        };
        m_data.UpdateInstanceData(handle, StlSpan<const InstanceMetadata>{tombstone}.AsBytes());
    }

    void Scene::Update(RHICommandList* cmd)
    {
        m_data.Update(cmd);
    }
}
