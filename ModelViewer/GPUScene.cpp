#include "GPUScene.hpp"
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

GPUScene::GPUScene(RHIDevice* device, size_t numSwaps, SceneBudgets const& budgets, Allocator* alloc) :
    mAllocator(alloc), mUpload(device, alloc, budgets.totalStaging()),
    mInstance(device, budgets.instanceBudget, budgets.instanceAlignment, numSwaps, alloc),
    mShared(device, budgets.sharedBudget, 4, numSwaps, alloc), mSharedAlloc(budgets.sharedBudget, alloc),
    mSharedUpdateRegions(alloc), mGeometryAlloc(budgets.geometryBudget, alloc)
{
    mGeometry =
        device->CreateBuffer({.usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
                              .size = budgets.geometryBudget});
}

VirtualAllocation GPUScene::PushShared(Span<const char> data, size_t alignment)
{
    std::scoped_lock lock(mShared.mutex);
    auto allocation = mSharedAlloc.Allocate(data.size_bytes(), alignment);
    auto offset = mSharedAlloc.QueryOffset(allocation);
    mSharedUpdateRegions.emplace_back(offset, data.size_bytes());
    std::memcpy(mShared.data + offset, data.data(), data.size_bytes());
    mSharedDirty = true;
    return allocation;
}
Pair<size_t, size_t> GPUScene::QueryShared(VirtualAllocation allocation)
{
    return mSharedAlloc.Query(allocation);
}
void GPUScene::UpdateShared(VirtualAllocation allocation, Span<const char> data)
{
    std::scoped_lock lock(mShared.mutex);
    auto [offset, size] = mSharedAlloc.Query(allocation);
    CHECK_MSG(size >= data.size_bytes(), "Data size too large (expected={}, got={})", size, data.size_bytes());
    mSharedUpdateRegions.emplace_back(offset, size);
    std::memcpy(mShared.data + offset, data.data(), data.size_bytes());
    mSharedDirty = true;
}
void GPUScene::FreeShared(const VirtualAllocation allocation) { mSharedAlloc.Free(allocation); }

void GPUScene::CreateUpdatePasses(Renderer* renderer, ResourceHandle& outInstanceBuffer,
                                  ResourceHandle& outSharedBuffer, ResourceHandle& outGeometryBuffer, RHIDeviceQueueType queue)
{
    createStagedBufferUpdatePass(renderer, &mInstance.buffer, "Instance Buffer", outInstanceBuffer, queue);
    createStagedBufferUpdatePass(renderer, &mShared.buffer, "Shared Buffer", outSharedBuffer, queue);
}
