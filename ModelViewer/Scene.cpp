#include "Scene.hpp"
#include <tracy/Tracy.hpp>
#include "Mesh.hpp"
using namespace ModelViewer;
using namespace Foundation::Async;
using namespace Foundation::Native;
StagedData::StagedData(RHIDevice* device, const size_t size, const size_t alignment, const size_t numSwaps,
                       Allocator* alloc) :
    alloc(alloc),
    buffer(device, alloc, numSwaps,
           {.usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination, .size = size}, size,
           ~0u),
    size(size), alignment(alignment), data(static_cast<char*>(alloc->Allocate(size, alignment)))
{
    CHECK_MSG(size % alignment == 0, "Bad alignment for size={}, alignment={}", size, alignment);
    std::memset(data, 0, size);
}
StagedData::~StagedData() { alloc->Deallocate(data); }

Scene::Scene(RHIDevice* device, size_t numSwaps, SceneBudgets const& budgets, Allocator* alloc) :
    mAllocator(alloc), mUpload(device, alloc, budgets.TotalBudget()),
    mInstance(device, budgets.InstanceBudget, budgets.InstanceAlignment, numSwaps, alloc),
    mMetadata(device, budgets.MetadataBudget, 4, numSwaps, alloc), mMetadataRegions(alloc), mMeshes(alloc),
    mPrimitiveAlloc(budgets.PrimitiveBudget, alloc), mVertexAlloc(budgets.VertexBudget, alloc),
    mIndexAlloc(budgets.IndexBudget, alloc), mMetadataAlloc(budgets.MetadataBudget, alloc)
{
    mPrimitive =
        device->CreateBuffer({.usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
                              .size = budgets.PrimitiveBudget});
    mVertex = device->CreateBuffer({.usage = RHIBufferUsageBits::VertexBuffer | RHIBufferUsageBits::TransferDestination,
                                    .size = budgets.VertexBudget});
    mIndex = device->CreateBuffer({.usage = RHIBufferUsageBits::IndexBuffer | RHIBufferUsageBits::TransferDestination,
                                   .size = budgets.IndexBudget});
}
void Scene::OnBeforeFrame(const uint32_t rendererSync)
{
    ZoneScopedN("Scene Update");
    {
        // Flush all pending uploads
        // This always blocks until completion.
        ZoneScopedN("Wait Uploads");
        mUpload.SubmitAndWait();
    }
    if (mInstanceDirty)
    {
        // Schedule the entirety of the instance buffer to be re-uploaded.
        // The backing StagedBuffer is N-buffered (renderSync), so we can do this without stalling the GPU.
        ZoneScopedN("Instance Data");
        {
            ZoneScopedN("Waiting for Mutex");
            mInstance.mutex.lock();
        }
        mInstanceDirty = false;
        mInstance.buffer.BeginTransfer(rendererSync);
        mInstance.buffer.Transfer(0, mInstance.View<char>(), mInstance.alignment);
        mInstance.buffer.EndTransfer();
        mInstance.mutex.unlock();
    }
    if (mMetadataDirty)
    {
        // Schedule regions of the metadata buffer to be re-uploaded.
        ZoneScopedN("Metadata Data");
        {
            ZoneScopedN("Waiting for Mutex");
            mMetadata.mutex.lock();
        }
        mMetadataDirty = false;
        mMetadata.buffer.BeginTransfer(rendererSync);
        for (auto [offset, size] : mMetadataRegions)
            mMetadata.buffer.Transfer(offset, Span<const char>(mMetadata.data + offset, size).AsBytes(), 4);
        mMetadataRegions.clear();
        mMetadata.buffer.EndTransfer();
        mMetadata.mutex.unlock();
    }
}
SceneHandle Scene::PushMesh(Span<const char> vertices, Span<const char> indices)
{
    auto [handle, data] = mMeshes.PopPair();
    data.primitive = mPrimitiveAlloc.Allocate(sizeof(PrimitiveMetadata), alignof(PrimitiveMetadata));
    data.primitiveOffset = mPrimitiveAlloc.QueryOffset(data.primitive);
    data.vertex = mVertexAlloc.Allocate(vertices.size_bytes(), 4);
    data.vertexOffset = mVertexAlloc.QueryOffset(data.vertex);
    data.index = mIndexAlloc.Allocate(indices.size_bytes(), 4);
    data.indexOffset = mIndexAlloc.QueryOffset(data.index);
    PrimitiveMetadata allocation{
        .vertexOffset = static_cast<int>(data.vertexOffset),
        .indexCount = static_cast<int>(indices.size() / sizeof(uint32_t)),
        .indexOffset = static_cast<int>(data.indexOffset),
    };
    mUpload.Upload(mPrimitive.Get(), Span<PrimitiveMetadata>(allocation).AsBytes(), data.primitiveOffset);
    mUpload.Upload(mVertex.Get(), vertices, data.vertexOffset);
    mUpload.Upload(mIndex.Get(), indices, data.indexOffset);
    return handle;
}
SceneMesh Scene::QueryMesh(SceneHandle id) { return mMeshes.At(id); }
void Scene::DestroyMesh(SceneHandle mesh)
{
    auto data = mMeshes.At(mesh);
    mPrimitiveAlloc.Free(data.primitive);
    mVertexAlloc.Free(data.vertex);
    mIndexAlloc.Free(data.index);
    mMeshes.Free(mesh);
}
VirtualAllocation Scene::PushMetadata(Span<const char> data, size_t alignment)
{
    std::scoped_lock lock(mMetadata.mutex);
    auto allocation = mMetadataAlloc.Allocate(data.size_bytes(), alignment);
    auto offset = mMetadataAlloc.QueryOffset(allocation);
    mMetadataRegions.emplace_back(offset, data.size_bytes());
    std::memcpy(mMetadata.data + offset, data.data(), data.size_bytes());
    mMetadataDirty = true;
    return allocation;
}
Pair<size_t, size_t> Scene::QueryMetadata(VirtualAllocation allocation)
{
    return mMetadataAlloc.Query(allocation);
}
void Scene::UpdateMetadata(VirtualAllocation allocation, Span<const char> data)
{
    std::scoped_lock lock(mMetadata.mutex);
    auto [offset, size] = mMetadataAlloc.Query(allocation);
    CHECK_MSG(size >= data.size_bytes(), "Data size too large (expected={}, got={})", size, data.size_bytes());
    mMetadataRegions.emplace_back(offset, size);
    std::memcpy(mMetadata.data + offset, data.data(), data.size_bytes());
    mMetadataDirty = true;
}
void Scene::FreeMetadata(const VirtualAllocation allocation)
{
    mMetadataAlloc.Free(allocation);
}
void Scene::CreateUpdatePasses(Renderer* renderer, ResourceHandle& outInstanceBuffer, ResourceHandle& outMetadataBuffer, RHIDeviceQueueType queue)
{
    createStagedBufferUpdatePass(renderer, &mInstance.buffer, "Instance Buffer", outInstanceBuffer, queue);
    createStagedBufferUpdatePass(renderer, &mMetadata.buffer, "Metadata Buffer", outMetadataBuffer, queue);
}
