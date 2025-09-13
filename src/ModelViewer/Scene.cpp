#include "Scene.hpp"
namespace Foundation {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    using namespace Foundation::Rendering;
    Scene::Scene(Allocator* allocator, RHIDevice* device, SceneDataDesc const& desc) :
        m_allocator(allocator), m_data(allocator, device, desc) {}

    DataHandle Scene::AddMeshSpan(Span<const Vertex> vertices, Span<const Index> indices, DataHandle& outVtx, DataHandle& outIdx) {
        DataHandle vtx = m_data.AddVertexData(vertices.AsBytes());
        DataHandle idx = m_data.AddIndexData(indices.AsBytes());
        PrimitiveMetadata prim{
            .vertexOffset = static_cast<int>(m_data.QueryVertexDataSizeAndOffset(vtx).second),
            .indexCount = static_cast<int>(indices.size()),
            .indexOffset = static_cast<int>(m_data.QueryIndexDataSizeAndOffset(idx).second),
            .sphereBounds = {} // !! TODO
        };
        outVtx = vtx, outIdx = idx;
        return m_data.AddPrimitiveData(Span<PrimitiveMetadata>{prim}.AsBytes());
    };
    void Scene::FreeMesh(MeshHandle handle) {
        auto [mesh, vtx, idx] = handle;
        // TODO: Ref-counting usages?
        m_data.FreePrimitiveData(mesh);
        m_data.FreeVertexData(vtx);
        m_data.FreeIndexData(idx);
    }
    DataHandle Scene::AddInstance(InstanceMetadata data)
    {
        m_dirty = true;
        DataHandle hdl = m_data.AddInstanceData(Span<InstanceMetadata>{data}.AsBytes());
        return hdl;
    }
    void Scene::UpdateInstance(DataHandle instance, InstanceMetadata const& data)
    {
        m_dirty = true;
        m_data.UpdateInstanceData(instance, Span<const InstanceMetadata>{data}.AsBytes());
    }
    void Scene::FreeInstance(DataHandle handle)
    {
        m_dirty = true;
        const InstanceMetadata tombstone = {
            .enabled = 0,
            .primitiveID = ~0u,
            .transform = {}
        };
        m_data.UpdateInstanceData(handle, Span<const InstanceMetadata>{tombstone}.AsBytes());
    }

    void Scene::Update(RHICommandList* cmd)
    {
        m_data.Update(cmd);
        m_dirty = false;
    }
}
