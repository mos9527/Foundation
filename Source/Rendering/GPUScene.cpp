#include "GPUScene.hpp"

using namespace Foundation::Rendering;
using namespace Foundation::Async;
using namespace Foundation::Native;
StagedDoubleBuffer::StagedDoubleBuffer(RHIDevice* device, size_t size, size_t stagingSize, const size_t alignment,
                       Allocator* alloc) :
    alloc(alloc),
    data(static_cast<char*>(alloc->Allocate(size, alignment))),
    size(size), alignment(alignment), buffer(device, alloc, 
           {.usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination, .size = size},
           stagingSize,
           ~0u)
{
    CHECK_MSG(size % alignment == 0, "Bad alignment for size={}, alignment={}", size, alignment);
    std::memset(data, 0, size);
}
StagedDoubleBuffer::~StagedDoubleBuffer() { alloc->Deallocate(data); }

GPUScene::GPUScene(RHIDevice* device, GPUSceneBudgets const& budgets, Allocator* alloc) :
    mAllocator(alloc),
    mInstance(device, budgets.instanceBudget, budgets.instanceBudget, budgets.instanceAlignment, alloc),
    mShared(device, budgets.sharedBudget, budgets.sharedStaging, 4, alloc), mSharedAlloc(budgets.sharedBudget, alloc),
    mSharedUpdateRegions(alloc), mConst(device, budgets.constBudget, budgets.constStaging, 4, alloc),
    mConstAlloc(budgets.constBudget, alloc), mConstUpdateRegions(alloc)
{
    mInstance.buffer.GetBuffer()->DebugSetObjectName("GPUScene Instance");
    mShared.buffer.GetBuffer()->DebugSetObjectName("GPUScene Shared");
    mConst.buffer.GetBuffer()->DebugSetObjectName("GPUScene Const");
}

VirtualAllocation GPUScene::PushShared(Span<const char> data, size_t alignment)
{
    std::scoped_lock lock(mShared.mutex);
    auto allocation = mSharedAlloc.Allocate(data.size_bytes(), alignment);
    auto offset = mSharedAlloc.QueryOffset(allocation);
    mSharedUpdateRegions.emplace_back(offset, data.size_bytes());
    std::memcpy(mShared.data + offset, data.data(), data.size_bytes());
    return allocation;
}
Pair<size_t, size_t> GPUScene::QueryShared(VirtualAllocation allocation) { return mSharedAlloc.Query(allocation); }
void GPUScene::UpdateShared(VirtualAllocation allocation, Span<const char> data)
{
    std::scoped_lock lock(mShared.mutex);
    auto [offset, size] = mSharedAlloc.Query(allocation);
    CHECK_MSG(size >= data.size_bytes(), "Data size too large (expected={}, got={})", size, data.size_bytes());
    mSharedUpdateRegions.emplace_back(offset, size);
    std::memcpy(mShared.data + offset, data.data(), data.size_bytes());
}
void GPUScene::FreeShared(const VirtualAllocation allocation) { mSharedAlloc.Free(allocation); }

VirtualAllocation GPUScene::PushConst(Span<const char> data, size_t alignment)
{
    std::scoped_lock lock(mConst.mutex);
    auto allocation = mConstAlloc.Allocate(data.size_bytes(), alignment);
    auto offset = mConstAlloc.QueryOffset(allocation);
    mConstUpdateRegions.emplace_back(offset, data.size_bytes());
    std::memcpy(mConst.data + offset, data.data(), data.size_bytes());
    return allocation;
}
Pair<size_t, size_t> GPUScene::QueryConst(VirtualAllocation allocation) { return mConstAlloc.Query(allocation); }
void GPUScene::UpdateConst(VirtualAllocation allocation, Span<const char> data)
{
    std::scoped_lock lock(mConst.mutex);
    auto [offset, size] = mConstAlloc.Query(allocation);
    CHECK_MSG(size >= data.size_bytes(), "Data size too large (expected={}, got={})", size, data.size_bytes());
    mConstUpdateRegions.emplace_back(offset, size);
    std::memcpy(mConst.data + offset, data.data(), data.size_bytes());
}
void GPUScene::FreeConst(const VirtualAllocation allocation) { mConstAlloc.Free(allocation); }

void GPUScene::CreateUpdatePasses(Renderer* renderer, ResourceHandle& outInstanceBuffer,
                                  ResourceHandle& outSharedBuffer, ResourceHandle& outConstBuffer,
                                  RHIDeviceQueueType queue)
{
    auto createStagedBufferUpdatePass =
        [&](StagedDoubleBuffer* stagedData, ResourceHandle& outBufferHandle, StringView name,
            bool isInstance = false, Vector<Pair<size_t, size_t>>* updateRegions = nullptr
            )
    {
        outBufferHandle = createResource(renderer, name, stagedData->buffer.GetBuffer());
        createPass(
            renderer, name, queue,
            /* setup */
            [=](PassHandle self, Renderer* r)
            {
                r->BindBufferCopyDst(self, outBufferHandle);
                if (!isInstance)
                {
                    // Ensure instance data gets transferred first - see @ref MapInstanceData
                    r->BindBufferCopySrc(self, outInstanceBuffer);
                }
            },
            /* record */
            [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                std::scoped_lock lock(stagedData->mutex);
                if (isInstance)
                {
                    CHECK(mInstanceDirty);
                    stagedData->buffer.BeginTransfer();
                    stagedData->buffer.Transfer(0, mInstance.View<char>(), mInstance.alignment);
                    stagedData->buffer.EndTransfer();
                    stagedData->buffer.Update(cmd);
                    mInstanceDirty = false;
                } else if (updateRegions)
                {
                    CHECK(!updateRegions->empty());
                    stagedData->buffer.BeginTransfer();
                    for (auto [offset, size] : *updateRegions)
                        stagedData->buffer.Transfer(offset, Span<const char>(stagedData->data + offset, size).AsBytes(), 4);
                    stagedData->buffer.EndTransfer();
                    stagedData->buffer.Update(cmd);
                    updateRegions->clear();
                }
                stagedData->buffer.Update(cmd);
            },
            /* skip */
            [=, this](PassHandle self, Renderer* r)
            {
                std::scoped_lock lock(stagedData->mutex);
                if (isInstance)
                    return !mInstanceDirty;
                CHECK(updateRegions);
                return updateRegions->empty();
            });
    };
    createStagedBufferUpdatePass(&mInstance, outInstanceBuffer, "Instance Buffer", true);
    // Dependent on Instance data
    createStagedBufferUpdatePass(&mShared, outSharedBuffer, "Shared Buffer", false, &mSharedUpdateRegions);
    createStagedBufferUpdatePass(&mConst, outConstBuffer, "Const Buffer", false, &mConstUpdateRegions);
}
