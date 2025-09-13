#include "Scene.hpp"
namespace Foundation
{
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    using namespace Foundation::Rendering;

    ResourceHandle Scene::CreateUpdatePass(Renderer* renderer, DataBuffer* data_buffer, StringView name, RHIDeviceQueueType queue)
    {
        ResourceHandle buffer = createResource(renderer, fmt::format("{} Buffer", name), data_buffer->GetBuffer());
        createPass(
            renderer, name, queue, [=, this](PassHandle self, Renderer* r) { r->BindBufferCopyDst(self, buffer); },
            [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
            { data_buffer->Update(cmd); },
            [=, this](PassHandle self, Renderer* r) { return !data_buffer->HasUpdates(); });
        return buffer;
    }
    Scene::Scene(RHIDevice* device, Allocator* allocator, uint32_t numSwaps, SceneBudgets const& budgets) :
        m_device(device), m_allocator(allocator), m_stagingBuffers(allocator), m_idleGuard(m_device)
    {
        // Create one staging buffer for each frame in flight.
        // This in turn utilizes the frame fences for synchronization.
        // A lower memory footprint option would be to do this with a single staging buffer
        // and wait for the resource fence before reusing it at the cost of a CPU stall.
        for (size_t i = 0; i < numSwaps; i++)
        {
            m_stagingBuffers.emplace_back(allocator, device, budgets.StagingBudget);
            m_stagingBuffers.back().GetBuffer()->DebugSetObjectName(fmt::format("Scene Staging Buffer {}", i).c_str());
        }
        m_instanceData = ConstructUnique<DataBuffer>(
            allocator, "Scene Instance Data Buffer", allocator, device, budgets.InstanceDataBudget,
            RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination, 0u);
        m_primitiveData = ConstructUnique<DataBuffer>(allocator, "Scene Primitive Data Buffer", allocator, device, budgets.PrimitiveDataBudget,
                                        RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination);
        m_vertexData =  ConstructUnique<DataBuffer>(allocator, "Scene Vertex Data Buffer", allocator, device, budgets.VertexDataBudget,
                                        RHIBufferUsageBits::VertexBuffer | RHIBufferUsageBits::TransferDestination);
        m_indexData = ConstructUnique<DataBuffer>(allocator, "Scene Index Data Buffer", allocator, device, budgets.IndexDataBudget,
                                        RHIBufferUsageBits::IndexBuffer | RHIBufferUsageBits::TransferDestination);
    }
    StagingBuffer* Scene::GetStagingBuffer()
    {
        CHECK_MSG(m_currentSync < m_stagingBuffers.size(), "Invalid current sync index {}", m_currentSync);
        return &m_stagingBuffers[m_currentSync];
    }
    void Scene::BeginTransfer(uint32_t rendererSync)
    {
        CHECK_MSG(m_state == State::Idle, "Scene is already in {} state", m_state);
        m_instanceData->Abort();
        m_primitiveData->Abort();
        m_vertexData->Abort();
        m_indexData->Abort();
        m_currentSync = rendererSync;
        m_stagingBuffers[m_currentSync].Reset();
        m_state = State::Transfer;
    }
    MeshHandle Scene::CreateMesh(Mesh const& mesh)
    {
        auto vert = m_vertexData->PushData(GetStagingBuffer(), mesh.m_vertex_data , alignof(VertexSL));
        auto idx = m_indexData->PushData(GetStagingBuffer(), mesh.m_index_data, alignof(IndexSL));
        PrimitiveMetadata prim_data{
            .vertexOffset = static_cast<int>(m_vertexData->Query(vert).second),
            .indexCount = static_cast<int>(mesh.m_index_data.size()),
            .indexOffset = static_cast<int>(m_indexData->Query(idx).second),
            .sphereBounds = {} // !! TODO
        };
        auto prim = m_primitiveData->PushData(GetStagingBuffer(), Span<PrimitiveMetadata>{prim_data}.AsBytes(), alignof(PrimitiveMetadata));
        return { prim, vert, idx };
    }
    void Scene::FreeMesh(MeshHandle const& handle)
    {
        auto [prim, vert, idx] = handle;
        m_primitiveData->FreeData(prim);
        m_vertexData->FreeData(vert);
        m_indexData->FreeData(idx);
    }
    SceneHandle Scene::CreateInstance(InstanceMetadata data)
    {
        auto instance = m_instanceData->PushData(GetStagingBuffer(), Span<InstanceMetadata>{data}.AsBytes(), alignof(InstanceMetadata));
        return instance;
    }
    void Scene::UpdateInstance(SceneHandle instance, InstanceMetadata const& data)
    {
        m_instanceData->UpdateData(GetStagingBuffer(), instance, Span<const InstanceMetadata>{data}.AsBytes(), alignof(InstanceMetadata));
    }
    void Scene::FreeInstance(SceneHandle handle)
    {
        const InstanceMetadata tombstone = {.enabled = 0};
        m_instanceData->UpdateData(GetStagingBuffer(), handle, Span<const InstanceMetadata>{tombstone}.AsBytes(),
                                   alignof(InstanceMetadata));
        m_instanceData->FreeData(handle);
    }
    void Scene::EndTransfer() {
        CHECK_MSG(m_state == State::Transfer, "Scene is not in Transfer state");
        m_state = State::Idle;
        m_currentSync = ~0u;
    }
} // namespace Foundation
