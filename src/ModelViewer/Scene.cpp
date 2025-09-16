#include "Scene.hpp"
#include "Mesh.hpp"
using namespace Foundation;
Scene::Scene(RHIDevice* device, Allocator* alloc, size_t numSwaps, SceneBudgets const& budgets) :
    m_allocator(alloc),
    m_instanceBuffer(device, alloc, numSwaps,
                     {.usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
                     .size = budgets.InstanceBudget},
                     budgets.InstanceStaging, 0u),
    m_primitiveBuffer(device, alloc, numSwaps,
                      { .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
                      .size = budgets.PrimitiveBudget},
                      budgets.PrimitiveStaging),
    m_vertexBuffer(device, alloc, numSwaps,
                   {.usage = RHIBufferUsageBits::VertexBuffer | RHIBufferUsageBits::TransferDestination,
                   .size = budgets.VertexBudget},
                   budgets.InstanceStaging),
    m_indexBuffer(device, alloc, numSwaps,
                  {.usage = RHIBufferUsageBits::IndexBuffer | RHIBufferUsageBits::TransferDestination,
                  .size = budgets.InstanceBudget},
                  budgets.InstanceStaging),
    m_instanceQueue(alloc), m_meshQueue(alloc), m_meshes(alloc)
{}
void Scene::OnBeforeFrame(uint32_t rendererSync)
{
    std::scoped_lock lock(m_updateMutex);
    CHECK_MSG(m_state == State::Idle, "Bad Scene State ({})", m_state);
    m_state = State::Update;
    BeginUpdate(rendererSync);
    while (!m_meshQueue.empty())
    {
        auto& [mutex, fpath, alloc] = m_meshQueue.front();
        // TODO: Bake the data and acquire metadata information for allocation patterns
        //       We'll assume this will always fit for a frame now - and load only one of them at a time.
        auto mesh = LoadMeshFromObjFile(fpath, m_allocator);
        alloc->vertex = m_vertexBuffer.Push(mesh.m_vertex_data);
        alloc->index = m_indexBuffer.Push(mesh.m_index_data);
        CHECK_MSG(alloc->vertex != kInvalidHandle && alloc->index != kInvalidHandle,
                  "Failed to allocate mesh data (vtx: {}, idx: {})", alloc->vertex, alloc->index);
        PrimitiveMetadata primitive{
            .vertexOffset = static_cast<int>(m_vertexBuffer.Query(alloc->vertex).second),
            .indexCount = static_cast<int>(mesh.m_index_data.size()),
            .indexOffset = static_cast<int>(m_indexBuffer.Query(alloc->index).second),
        };
        auto [prim_size, prim_offset] =
            m_primitiveBuffer.Query(m_primitiveBuffer.Push(Span<PrimitiveMetadata>(primitive).AsBytes()));
        CHECK(prim_size == sizeof(PrimitiveMetadata));
        alloc->primitiveID = prim_offset / prim_size;
        mutex->unlock();
        m_meshQueue.pop();
        break; // TODO: Batch more!
    }
    while (!m_instanceQueue.empty())
    {
        // Instance data is always of the same sizes
        // and always pre-allocated.
        auto& [handle, data] = m_instanceQueue.front();
        static_assert(sizeof(data) == sizeof(InstanceMetadata));
        size_t offset = sizeof(data) * handle;
        m_instanceBuffer.Transfer(offset, Span<InstanceMetadata>(data).AsBytes());
        m_instanceQueue.pop();
    }
    EndUpdate();
    m_state = State::Idle;
}
void Scene::BeginUpdateAsync()
{
    m_updateMutex.lock();
    m_state = State::UpdateAsync;
}
SceneFuture Scene::LoadMeshAsync(Path path)
{
    CHECK_MSG(m_state == State::UpdateAsync, "Bad Scene State ({})", m_state);
    auto mutex = ConstructShared<Mutex>(m_allocator);
    mutex->lock();
    auto& data = m_meshes.emplace_back();
    m_meshQueue.emplace(mutex, path, &data);
    return SceneFuture(this, mutex, &data);
}
void Scene::UpdateInstanceAsync(SceneHandle id, InstanceMetadata data)
{
    CHECK_MSG(m_state == State::UpdateAsync, "Bad Scene State ({}). This must be called within a BeginUpdateAsync() and EndUpdateAsync() clause.", m_state);
    m_instanceQueue.emplace(id, data);
}
void Scene::EndUpdateAsync()
{
    m_state = State::Idle;
    m_updateMutex.unlock();
}
void SceneFuture::wait() const {
    CHECK_MSG(
        scene->GetState() != Scene::State::UpdateAsync,
        "Deadlock detected (state={}). This future must be waited _after_ the Scene's EndUpdateAsync() call.",
        scene->GetState()
    );
    std::scoped_lock lock(*mutex);
}
void Scene::BeginUpdate(uint32_t rendererSync)
{
    m_instanceBuffer.BeginTransfer(rendererSync);
    m_primitiveBuffer.BeginTransfer(rendererSync);
    m_vertexBuffer.BeginTransfer(rendererSync);
    m_indexBuffer.BeginTransfer(rendererSync);
}
void Scene::EndUpdate()
{
    m_instanceBuffer.EndTransfer();
    m_primitiveBuffer.EndTransfer();
    m_vertexBuffer.EndTransfer();
    m_indexBuffer.EndTransfer();
}
void Scene::CreateUpdatePasses(Renderer* renderer, ResourceHandle& outInstance, ResourceHandle& outPrimitive,
                               ResourceHandle& outVertex, ResourceHandle& outIndex, RHIDeviceQueueType queue)
{
    createStagedBufferUpdatePass(renderer, &m_instanceBuffer, "Instance", outInstance, queue);
    createStagedBufferUpdatePass(renderer, &m_primitiveBuffer, "Primitive", outPrimitive, queue);
    createStagedBufferUpdatePass(renderer, &m_vertexBuffer, "Vertex", outVertex, queue);
    createStagedBufferUpdatePass(renderer, &m_indexBuffer, "Index", outIndex, queue);
}
