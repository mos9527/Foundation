#include "Scene.hpp"
#include "Mesh.hpp"
#include <tracy/Tracy.hpp>
using namespace ModelViewer;
using namespace Foundation::Async;
using namespace Foundation::Native;
constexpr size_t kMaxQueuedInstanceUpdates = 65536, kMaxQueuedMeshLoads = 16;
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
                   budgets.VertexStaging),
    m_indexBuffer(device, alloc, numSwaps,
                  {.usage = RHIBufferUsageBits::IndexBuffer | RHIBufferUsageBits::TransferDestination,
                  .size = budgets.IndexBudget},
                  budgets.IndexStaging),
    m_instanceQueue( kMaxQueuedInstanceUpdates, alloc), m_meshQueue(kMaxQueuedMeshLoads, alloc), m_meshes(alloc)
{}
void Scene::OnBeforeFrame(uint32_t rendererSync)
{
    ZoneScoped;
    CHECK_MSG(m_state == State::Idle, "Bad Scene State ({})", m_state);
    m_state = State::Update;
    BeginUpdate(rendererSync);
    while (true)
    {
        // TODO: No one loads meshes _every_ frame. Sparse updates don't really need a
        //       staging buffer - blocking updates are fine. Transfers are not _that_ slow
        //       if batched properly.
        auto value = m_meshQueue.pop();
        if (!value.has_value())
            break;
        auto& [mutex, fpath, alloc] = value.value();
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
        // TODO: MSVC dies here if we'd do a Debug build.
        mutex->unlock();
        m_meshQueue.pop();
        break; // TODO: Batch more!
    }
    size_t instanceUpd = 0;
    {
        // TODO: Second thoughts. This is quite a bottleneck if we have many instances to update.
        //       Perf is _not_ good. Considering the whole instance data size to be quite small,
        //       maybe we should just copy the whole thing by maintaining one on the CPU too?
        ZoneScopedN("Instance Update");
        while (true)
        {
            // Instance data is always of the same sizes
            // and always pre-allocated.
            auto value = m_instanceQueue.pop();
            if (!value.has_value())
                break;
            auto& [handle, data] = value.value();
            static_assert(sizeof(data) == sizeof(InstanceMetadata));
            size_t offset = sizeof(data) * handle;
            if (m_instanceBuffer.GetStagingBuffer()->FreeSize() < sizeof(data))
            {
                // Defer to next frame
                LOG_RUNTIME(Scene, warn, "Instance update being deferred. Done {}. Increase staging size to avoid latency!", instanceUpd);
                break;
            }
            m_instanceBuffer.Transfer(offset, Span<InstanceMetadata>(data).AsBytes());
            m_instanceQueue.pop();
            instanceUpd++;
        }
    }
    EndUpdate();
    m_state = State::Idle;
}

SceneFuture Scene::LoadMeshAsync(Path path)
{
    auto mutex = ConstructShared<Mutex>(m_allocator);
    mutex->lock();
    auto& data = m_meshes.emplace_back();
    m_meshQueue.push({mutex, path, &data});
    return SceneFuture(this, mutex, &data);
}
void Scene::UpdateInstanceAsync(SceneHandle id, InstanceMetadata data)
{
    m_instanceQueue.push({id, data});
}
void SceneFuture::wait() const {
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
