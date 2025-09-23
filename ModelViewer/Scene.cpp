#include "Scene.hpp"
#include "Mesh.hpp"
#include <tracy/Tracy.hpp>
using namespace ModelViewer;
using namespace Foundation::Async;
using namespace Foundation::Native;
StagedData::StagedData(RHIDevice* device, const size_t size, const size_t alignment, const size_t numSwaps, Allocator* alloc) :
    alloc(alloc),
    buffer(device, alloc, numSwaps,
           {.usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination, .size = size}, size,
           0u),
    size(size), alignment(alignment), data(static_cast<char*>(alloc->Allocate(size, alignment)))
{
    CHECK_MSG(size % alignment == 0, "Bad alignment for size={}, alignment={}", size, alignment);
}
StagedData::~StagedData()
{
    alloc->Deallocate(data);
}

Scene::Scene(RHIDevice* device, size_t numSwaps, SceneBudgets const& budgets, Allocator* alloc) :
    m_allocator(alloc),
    m_upload(device, alloc, budgets.TotalBudget()),
    m_instance(device, budgets.InstanceBudget, budgets.InstanceAlignment, numSwaps, alloc),
    m_meshes(kMaxSceneMeshes, alloc)
{
    m_prmitive =
        device->CreateBuffer({.usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
                              .size = budgets.PrimitiveBudget});
    m_vertex = device->CreateBuffer(
        {.usage = RHIBufferUsageBits::VertexBuffer | RHIBufferUsageBits::TransferDestination, .size = budgets.VertexBudget});
    m_index = device->CreateBuffer(
        {.usage = RHIBufferUsageBits::IndexBuffer | RHIBufferUsageBits::TransferDestination, .size = budgets.IndexBudget});
}
void Scene::OnBeforeFrame(const uint32_t rendererSync)
{
    ZoneScopedN("Scene Update");
    {
        // Flush all pending uploads
        // This always blocks until completion.
        ZoneScopedN("Wait Uploads");
        m_upload.SubmitAndWait();
    }
    if (m_instanceDirty) {
        // Schedule the entirety of the instance buffer to be re-uploaded.
        // The backing StagedBuffer is N-buffered (renderSync), so we can do this without stalling the GPU.
        ZoneScopedN("Instance Data");
        {
            ZoneScopedN("Waiting for Mutex");
            m_instance.mutex.lock();
        }
        m_instanceDirty = false;
        m_instance.buffer.BeginTransfer(rendererSync);
        m_instance.buffer.Transfer(0, m_instance.View<char>(), m_instance.alignment);
        m_instance.buffer.EndTransfer();
        m_instance.mutex.unlock();
    }
}
void Scene::UploadAllocation(RHIBuffer* buffer, Span<const char> data, BufferAllocation allocation)
{
    auto& arena = buffer->GetArena();
    auto size = arena.GetSize(allocation);
    auto offset = arena.GetOffset(allocation);
    m_upload.Upload(buffer, data, offset, size);
}
SceneHandle Scene::CreateMesh(Span<const char> vertices, Span<const char> indices)
{
    auto [handle, data] = m_meshes.pop_pair();
    auto primitive = m_prmitive->GetArena().Allocate(sizeof(PrimitiveMetadata), alignof(PrimitiveMetadata));
    data.primitiveID = m_prmitive->GetArena().GetOffset(primitive) / sizeof(PrimitiveMetadata);
    data.vertex = m_vertex->GetArena().Allocate(vertices.size_bytes(), 4);
    data.index = m_index->GetArena().Allocate(indices.size_bytes(), 4);
    PrimitiveMetadata allocation{
        .vertexOffset = static_cast<int>(m_vertex->GetArena().GetOffset(data.vertex)),
        .indexCount = static_cast<int>(indices.size() / sizeof(uint32_t)), // ! TODO
        .indexOffset = static_cast<int>(m_index->GetArena().GetOffset(data.index)),
    };
    UploadAllocation(m_prmitive.Get(), Span<PrimitiveMetadata>(allocation).AsBytes(), primitive);
    UploadAllocation(m_vertex.Get(), vertices, data.vertex);
    UploadAllocation(m_index.Get(), indices, data.index);
    return handle;
}
SceneMesh Scene::GetMesh(SceneHandle id)
{
    return m_meshes.at(id);
}
void Scene::DestroyMesh(SceneHandle mesh)
{
    auto data = m_meshes.at(mesh);
    m_vertex->GetArena().Free(data.vertex);
    m_index->GetArena().Free(data.index);
    m_meshes.free(mesh);
}
void Scene::CreateInstanceUpdatePass(Renderer* renderer, ResourceHandle& outInstanceBuffer, RHIDeviceQueueType queue)
{
    createStagedBufferUpdatePass(renderer, &m_instance.buffer, "Instance Buffer", outInstanceBuffer, queue);
}