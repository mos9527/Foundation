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
           0u),
    size(size), alignment(alignment), data(static_cast<char*>(alloc->Allocate(size, alignment)))
{
    CHECK_MSG(size % alignment == 0, "Bad alignment for size={}, alignment={}", size, alignment);
}
StagedData::~StagedData() { alloc->Deallocate(data); }

Scene::Scene(RHIDevice* device, size_t numSwaps, SceneBudgets const& budgets, Allocator* alloc) :
    mAllocator(alloc), mUpload(device, alloc, budgets.TotalBudget()),
    mInstance(device, budgets.InstanceBudget, budgets.InstanceAlignment, numSwaps, alloc), mMeshes(alloc),
    mPrimitiveAlloc(budgets.PrimitiveBudget, alloc), mVertexAlloc(budgets.VertexBudget, alloc),
    mIndexAlloc(budgets.IndexBudget, alloc)
{
    mPrimitive =
        device->CreateBuffer({.usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
                              .size = budgets.PrimitiveBudget});
    mVertex =
        device->CreateBuffer({.usage = RHIBufferUsageBits::VertexBuffer | RHIBufferUsageBits::TransferDestination,
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
}
SceneHandle Scene::CreateMesh(Span<const char> vertices, Span<const char> indices)
{
    auto [handle, data] = mMeshes.pop_pair();
    auto primitive = mPrimitiveAlloc.Allocate(sizeof(PrimitiveMetadata), alignof(PrimitiveMetadata));
    data.primitiveID = mPrimitiveAlloc.QueryOffset(primitive) / sizeof(PrimitiveMetadata);
    data.vertex = mVertexAlloc.Allocate(vertices.size_bytes(), 4);
    data.index = mIndexAlloc.Allocate(indices.size_bytes(), 4);
    PrimitiveMetadata allocation{
        .vertexOffset = static_cast<int>(mVertexAlloc.QueryOffset(data.vertex)),
        .indexCount = static_cast<int>(indices.size() / sizeof(uint32_t)), // ! TODO
        .indexOffset = static_cast<int>(mIndexAlloc.QueryOffset(data.index)),
    };
    mUpload.Upload(mPrimitive.Get(), Span<PrimitiveMetadata>(allocation).AsBytes(), mPrimitiveAlloc.QueryOffset(primitive));
    mUpload.Upload(mVertex.Get(), vertices, mVertexAlloc.QueryOffset(data.vertex));
    mUpload.Upload(mIndex.Get(), indices, mIndexAlloc.QueryOffset(data.index));
    return handle;
}
SceneMesh Scene::GetMesh(SceneHandle id) { return mMeshes.at(id); }
void Scene::DestroyMesh(SceneHandle mesh)
{
    auto data = mMeshes.at(mesh);
    mPrimitiveAlloc.Free(data.primitiveID);
    mVertexAlloc.Free(data.vertex);
    mIndexAlloc.Free(data.index);
    mMeshes.free(mesh);
}
void Scene::CreateInstanceUpdatePass(Renderer* renderer, ResourceHandle& outInstanceBuffer, RHIDeviceQueueType queue)
{
    createStagedBufferUpdatePass(renderer, &mInstance.buffer, "Instance Buffer", outInstanceBuffer, queue);
}
