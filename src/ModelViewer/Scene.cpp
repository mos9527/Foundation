#include "Scene.hpp"
namespace Foundation {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    using namespace Foundation::Rendering;
    Scene::Scene(Allocator* allocator, RHIDevice* device, SceneDataDesc const& desc) :
        m_allocator(allocator), m_data(allocator, device, desc),
        m_instanceOffsets(allocator) {}

    template<typename Vertex, typename Index>
    SceneHandle Scene::AddMesh(StlSpan<const Vertex> vertices, StlSpan<const Index> indices) {
        StlVector<uint8_t> buffer(sizeof(MeshMetadata) + vertices.size_bytes() + indices.size_bytes(), m_allocator);
        MeshMetadata mesh{
            .vertexCount = vertices.size(),
            .vertexStride = sizeof(Vertex),
            .indexCount = indices.size(),
            .indexStride = sizeof(Index)
        };
        uint8_t* data = buffer.data();
        std::memcpy(data, &mesh, sizeof(mesh));
        data += sizeof(mesh);
        std::memcpy(data, vertices.data(), vertices.size_bytes());
        data += vertices.size_bytes();
        std::memcpy(data, indices.data(), indices.size_bytes());
        return m_data.AddPrimitiveData(buffer);
    };
    void Scene::FreeMesh(SceneHandle handle) {
        m_data.FreePrimitiveData(handle);
    }
    template<typename Instance>
    SceneHandle Scene::AddInstance(SceneHandle primitive, Instance const& idata) {
        InstanceMetadata instance{
            .primitiveOffset = m_data.QueryPrimitiveDataSizeAndOffset(primitive).second
        };
        uint8_t buffer[sizeof(InstanceMetadata) + sizeof(idata)];
        uint8_t* data = buffer;
        std::memcpy(data, &instance, sizeof(instance));
        data += sizeof(instance);
        std::memcpy(data, &idata, sizeof(idata));
        SceneHandle ihdl = m_data.AddInstanceData(buffer);
        auto [sz, off] = m_data.QueryInstanceDataSizeAndOffset(ihdl);
        m_instanceOffsets.push_back(off);
        m_hasDirtyInstance = true;
        return ihdl;
    }
    template<typename Instance>
    void Scene::UpdateInstance(SceneHandle ihdl, SceneHandle primitive, Instance const& idata) {
        auto [sz, off] = m_data.QueryInstanceDataSizeAndOffset(ihdl);
        InstanceMetadata instance{
            .primitiveOffset = m_data.QueryPrimitiveDataSizeAndOffset(primitive).second
        };
        uint8_t buffer[sizeof(InstanceMetadata) + sizeof(idata)];
        uint8_t* data = buffer;
        std::memcpy(data, &instance, sizeof(instance));
        data += sizeof(instance);
        std::memcpy(data, &idata, sizeof(idata));
        m_data.UpdateInstanceData(instance, buffer);
        m_hasDirtyInstance = true;
    }
    void Scene::FreeInstance(SceneHandle handle) {
        m_data.FreeInstanceData(handle);
        // XXX: Worst case O(N). Quite unfortunate, though we need
        // the data to be densely packed. This should be amortized as much as possible.
        auto it = std::find(m_instanceOffsets.begin(), m_instanceOffsets.end(), handle);
        m_instanceOffsets.erase(it);
        m_hasDirtyInstance = true;
    }

    template<typename Global>
    void Scene::UpdateGlobal(Global const& gdata) {
        size_t gsize = sizeof(GlobalMetadata) + sizeof(uint64_t) * m_instanceOffsets.size() + sizeof(gdata);
        CHECK(gsize <= m_data.GetGlobalDataBuffer()->m_desc.size && "Global data overflow");
        uint8_t* data = reinterpret_cast<uint8_t*>(m_data.GetGlobalDataBuffer()->Map());
        std::memcpy(data, &gdata, sizeof(gdata));
        data += sizeof(gdata);
        GlobalMetadata meta{
            .instanceCount = m_instanceOffsets.size()
        };
        std::memcpy(data, &meta, sizeof(meta));
        data += sizeof(meta);
        if (m_hasDirtyInstance) {
            std::memcpy(data, m_instanceOffsets.data(), sizeof(uint64_t) * m_instanceOffsets.size());
            m_hasDirtyInstance = false;
        }
    }
}
