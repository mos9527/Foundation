#pragma once
#include <Core/Container.hpp>
#include <Core/Logging.hpp>
#include <RHICore/Device.hpp>
#include <algorithm>
#include <cstring>
#include "Shaders/GPUScenePaging.h"

// Internal GPUScene storage. Scene commits and CPU dynamic edits are serialized with
// render-graph recording; TLAS uploads are allocated by the AS recording pass. Geometry
// allocation/resolution is additionally serialized by GPUScene's residency lock.
namespace GPUSceneBuffers
{
using namespace Foundation::Core;
using namespace Foundation::RHI;

// Retire upload snapshots against actual GPU progress, not a guessed number of frames
// or scene commits. Several editor commits can occur before a single frame is submitted.
// Before retiring a snapshot, all command lists that reference it must have been submitted.
class Retirement
{
    struct Queue
    {
        RHIDeviceQueue* queue;
        RHIDeviceScopedHandle<RHIDeviceSemaphore> timeline;
    };
    RHIDevice* mDevice;
    Vector<Queue> mQueues;
    size_t mNextValue{0};
    size_t mCheckpoint{0};
    size_t mCompleted{0};

public:
    Retirement(RHIDevice* device, Allocator* allocator) : mDevice(device), mQueues(allocator)
    {
        for (auto type : {RHIDeviceQueueType::Graphics, RHIDeviceQueueType::Compute, RHIDeviceQueueType::Transfer})
            if (auto* queue = device->GetDeviceQueue(type))
                mQueues.push_back({queue, device->CreateSemaphore(true)});
    }

    // Share one checkpoint across all snapshots replaced by this scene/update operation.
    void BeginEpoch() { mCheckpoint = 0; }

    size_t Checkpoint()
    {
        if (mCheckpoint != 0)
            return mCheckpoint;
        mCheckpoint = ++mNextValue;
        for (auto const& queue : mQueues)
        {
            RHIDeviceQueue::TimelinePair signal{queue.timeline.Get(), mCheckpoint};
            RHIDeviceQueue::SubmitDesc submit{.timelineSignals = {&signal, 1}};
            queue.queue->Submit({&submit, 1});
        }
        return mCheckpoint;
    }

    bool IsComplete(size_t value)
    {
        if (value <= mCompleted)
            return true;
        for (auto const& queue : mQueues)
        {
            RHIDeviceQueue::TimelinePair wait{queue.timeline.Get(), value};
            if (!mDevice->WaitForTimelineSemaphores({&wait, 1}, 0))
                return false;
        }
        mCompleted = value;
        return true;
    }
};

// Virtual offsets are in T elements. Every allocation resides in one physical buffer;
// large allocations get a multi-page buffer so CPU uploads and AS inputs stay contiguous.
// Physical buffers and page-table entries never move. Clear/Free reuse storage without
// rewriting mappings; their caller is responsible for retiring all GPU references first.
template <typename T, uint32_t PageShift>
class PagedBuffer
{
    static_assert(PageShift > 0 && PageShift + GS_PAGE_TABLE_LEAF_SHIFT < 32);
    static constexpr uint32_t kPageUnits = 1u << PageShift;
    static constexpr uint32_t kRootEntries = 1u << (32u - PageShift - GS_PAGE_TABLE_LEAF_SHIFT);
    struct Page
    {
        RHIDeviceScopedHandle<RHIBuffer> buffer;
        RHIDeviceScopedHandle<RHIVirtualAllocator> allocator;
        uint32_t base;
        uint32_t capacity;
    };
    RHIDevice* mDevice;
    RHIBufferDesc mDesc;
    String mDebugName;
    uint64_t mMinPageUnits;
    uint64_t mCapacity{0};
    RHIDeviceScopedHandle<RHIBuffer> mRoot;
    Vector<RHIDeviceScopedHandle<RHIBuffer>> mLeaves;
    Vector<Page> mPages;
    Vector<uint32_t> mPageOwners;

    RHIDeviceScopedHandle<RHIBuffer> CreateAddressTable(uint32_t entries, bool root, uint32_t index = 0)
    {
        auto buffer = mDevice->CreateBuffer(
            {.resource = {.heap = RHIDeviceHeapType::Upload,
                          .hostAccess = RHIResourceHostAccess::WriteOnly,
                          .shared = true,
                          .coherent = true},
             .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
                 RHIBufferUsageBits::TransferSource | RHIBufferUsageBits::TransferDestination,
             .size = size_t(entries) * sizeof(uint64_t),
             .alignment = alignof(uint64_t),
             .deviceAddressTable = root});
        std::memset(buffer->Map(), 0, buffer->mDesc.size);
        buffer->DebugSetObjectName((root ? Format("GPUScene / {} / Page Table Root", mDebugName)
                                        : Format("GPUScene / {} / Page Table Leaf {}", mDebugName, index)).c_str());
        return buffer;
    }

    Page const& PageFor(uint32_t offset) const
    {
        CHECK_MSG(offset != UINT32_MAX && offset < mCapacity, "Invalid GPUScene virtual offset {}", offset);
        return mPages[mPageOwners[offset >> PageShift]];
    }

public:
    struct PhysicalRange
    {
        RHIBuffer* buffer;
        size_t offset; // bytes within buffer, never a virtual offset
    };

    struct Statistics
    {
        size_t physicalPages{0};
        size_t logicalPages{0};
        size_t leafTables{0};
        size_t logicalPageBytes{0};
        size_t capacityBytes{0};
        size_t payloadBytes{0}; // Device allocation sizes, including allocator padding
        size_t pageTableBytes{0}; // Root plus allocated leaves
        size_t liveBytes{0}; // Active suballocations, including any upload alignment padding
        size_t retiredBytes{0}; // Superseded suballocations not yet reclaimed
        size_t retiredAllocations{0};
    };

    PagedBuffer(RHIDevice* device, Allocator* allocator, size_t minPageBytes, RHIBufferDesc desc,
                const char* debugName) :
        mDevice(device), mDesc(desc), mDebugName(debugName),
        mMinPageUnits((uint64_t(minPageBytes) + sizeof(T) - 1u) / sizeof(T)),
        mLeaves(allocator), mPages(allocator), mPageOwners(allocator)
    {
        CHECK_MSG(minPageBytes != 0, "GPUScene page size must be non-zero");
        mDesc.resource.shared = true;
        mDesc.usage |= RHIBufferUsageBits::DeviceAddress;
        mRoot = CreateAddressTable(kRootEntries, true);
    }

    ~PagedBuffer() { Clear(); }
    PagedBuffer(PagedBuffer const&) = delete;
    PagedBuffer& operator=(PagedBuffer const&) = delete;

    uint32_t Allocate(uint64_t count, uint32_t alignment = 1u)
    {
        if (count == 0 || count > UINT32_MAX)
            return UINT32_MAX;
        CHECK_MSG(alignment != 0 && (alignment & (alignment - 1u)) == 0 && alignment <= kPageUnits,
                  "Unsupported GPUScene allocation alignment {}", alignment);
        for (auto& page : mPages)
        {
            uint64_t local = page.allocator->Allocate(count, alignment);
            if (local != RHIVirtualAllocator::kInvalidOffset)
                return page.base + static_cast<uint32_t>(local);
        }
        uint64_t capacity = (std::max(count, mMinPageUnits) + kPageUnits - 1u) & ~uint64_t(kPageUnits - 1u);
        // UINT32_MAX remains the invalid offset. Address-space exhaustion is distinct from
        // a scene budget; page growth is otherwise limited only by device allocation success.
        if (capacity > UINT32_MAX - mCapacity || capacity > SIZE_MAX / sizeof(T))
            return UINT32_MAX;
        RHIBufferDesc desc = mDesc;
        desc.size = static_cast<size_t>(capacity) * sizeof(T);
        auto buffer = mDevice->CreateBuffer(desc);
        buffer->DebugSetObjectName(Format("GPUScene / {} / Page {} ({} bytes, logical pages {}..{})",
                                         mDebugName, mPages.size(), desc.size, mCapacity >> PageShift,
                                         ((mCapacity + capacity) >> PageShift) - 1u).c_str());
        auto allocator = mDevice->CreateVirtualAllocator(capacity);
        uint64_t local = allocator->Allocate(count, alignment);
        CHECK(local != RHIVirtualAllocator::kInvalidOffset);
        uint32_t base = static_cast<uint32_t>(mCapacity);
        uint64_t address = buffer->GetBufferAddress();
        uint32_t owner = static_cast<uint32_t>(mPages.size());
        mPages.push_back({std::move(buffer), std::move(allocator), base, static_cast<uint32_t>(capacity)});
        uint32_t firstPage = base >> PageShift;
        uint32_t numPages = static_cast<uint32_t>(capacity >> PageShift);
        mPageOwners.resize(size_t(firstPage) + numPages, owner);
        uint64_t* root = mRoot->Map<uint64_t>();
        for (uint32_t i = 0; i < numPages; ++i)
        {
            uint32_t pageIndex = firstPage + i;
            uint32_t leafIndex = pageIndex >> GS_PAGE_TABLE_LEAF_SHIFT;
            if (leafIndex >= mLeaves.size())
                mLeaves.resize(size_t(leafIndex) + 1u);
            if (!mLeaves[leafIndex])
            {
                mLeaves[leafIndex] = CreateAddressTable(GS_PAGE_TABLE_LEAF_ENTRIES, false, leafIndex);
                root[leafIndex] = mLeaves[leafIndex]->GetBufferAddress();
            }
            mLeaves[leafIndex]->Map<uint64_t>()[pageIndex & (GS_PAGE_TABLE_LEAF_ENTRIES - 1u)] =
                address + uint64_t(i) * kPageUnits * sizeof(T);
        }
        mCapacity += capacity;
        return base + static_cast<uint32_t>(local);
    }

    [[nodiscard]] PhysicalRange Resolve(uint32_t offset, uint64_t count = 1u) const
    {
        auto const& page = PageFor(offset);
        uint32_t local = offset - page.base;
        CHECK_MSG(count <= uint64_t(page.capacity) - local, "GPUScene range crosses physical allocations");
        return {page.buffer.Get(), size_t(local) * sizeof(T)};
    }

    [[nodiscard]] T* Mapped(uint32_t offset, uint64_t count = 1u) const
    {
        auto range = Resolve(offset, count);
        CHECK(range.buffer->mDesc.resource.hostAccess != RHIResourceHostAccess::Invisible);
        return reinterpret_cast<T*>(range.buffer->template Map<char>() + range.offset);
    }

    void Free(uint32_t offset)
    {
        auto const& page = PageFor(offset);
        page.allocator->Free(offset - page.base);
    }

    void Clear()
    {
        for (auto& page : mPages)
            page.allocator->Clear();
    }

    void Flush()
    {
        for (auto const& page : mPages)
            if (uint64_t used = page.allocator->GetPeakUsage())
                page.buffer->Flush(0, static_cast<size_t>(used) * sizeof(T));
    }

    [[nodiscard]] String const& GetDebugName() const { return mDebugName; }
    [[nodiscard]] Statistics GetStatistics() const
    {
        Statistics stats{
            .physicalPages = mPages.size(),
            .logicalPages = mPageOwners.size(),
            .logicalPageBytes = size_t(kPageUnits) * sizeof(T),
            .capacityBytes = static_cast<size_t>(mCapacity) * sizeof(T),
            .pageTableBytes = mRoot->GetAllocationSize(),
        };
        for (auto const& leaf : mLeaves)
            if (leaf)
            {
                ++stats.leafTables;
                stats.pageTableBytes += leaf->GetAllocationSize();
            }
        for (auto const& page : mPages)
        {
            stats.payloadBytes += page.buffer->GetAllocationSize();
            stats.liveBytes += static_cast<size_t>(page.allocator->GetUsedBytes()) * sizeof(T);
        }
        return stats;
    }

    [[nodiscard]] RHIBuffer* GetPageTable() const { return mRoot.Get(); }
    [[nodiscard]] uint32_t Capacity() const { return static_cast<uint32_t>(mCapacity); }
    [[nodiscard]] size_t GetUsedBytes() const
    {
        size_t result = 0;
        for (auto const& page : mPages)
            result += static_cast<size_t>(page.allocator->GetUsedBytes()) * sizeof(T);
        return result;
    }
    [[nodiscard]] size_t GetAllocationSize() const
    {
        size_t result = mRoot->GetAllocationSize();
        for (auto const& leaf : mLeaves)
            if (leaf)
                result += leaf->GetAllocationSize();
        for (auto const& page : mPages)
            result += page.buffer->GetAllocationSize();
        return result;
    }
};

// Host-visible snapshots retain their current allocation even on unchanged scene commits.
// Only superseded snapshots are reclaimed, and only after all prior queue work completes.
template <typename T, uint32_t PageShift = GS_TABLE_PAGE_SHIFT>
class UploadBuffer : public PagedBuffer<T, PageShift>
{
    struct Retired
    {
        uint32_t offset;
        size_t checkpoint;
    };
    Retirement& mRetirement;
    Vector<uint32_t> mCurrent;
    Vector<Retired> mRetired;
    uint32_t mUsed{0};

    void Reclaim()
    {
        auto first = std::remove_if(mRetired.begin(), mRetired.end(), [&](Retired const& retired)
        {
            if (!mRetirement.IsComplete(retired.checkpoint))
                return false;
            this->Free(retired.offset);
            return true;
        });
        mRetired.erase(first, mRetired.end());
    }

public:
    UploadBuffer(RHIDevice* device, Allocator* allocator, Retirement& retirement, size_t pageBytes,
                 const char* debugName) :
        PagedBuffer<T, PageShift>(device, allocator, pageBytes,
            {.resource = {.heap = RHIDeviceHeapType::Upload,
                          .hostAccess = RHIResourceHostAccess::WriteOnly,
                          .shared = true,
                          .coherent = true},
             .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
                 RHIBufferUsageBits::TransferSource | RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
             .alignment = std::max<size_t>(16u, alignof(T))}, debugName),
        mRetirement(retirement), mCurrent(allocator), mRetired(allocator)
    {
    }

    void BeginUpdate()
    {
        if (!mCurrent.empty())
        {
            size_t checkpoint = mRetirement.Checkpoint();
            for (uint32_t offset : mCurrent)
                mRetired.push_back({offset, checkpoint});
            mCurrent.clear();
        }
        mUsed = 0;
        Reclaim();
    }

    Pair<T*, uint32_t> AllocateMore(uint32_t count, uint32_t alignment = 1u)
    {
        if (count == 0)
            return {nullptr, 0u};
        uint32_t offset = PagedBuffer<T, PageShift>::Allocate(count, alignment);
        CHECK_MSG(offset != UINT32_MAX, "GPUScene upload virtual address space exhausted ({} elements)", count);
        mCurrent.push_back(offset);
        CHECK(count <= UINT32_MAX - mUsed);
        mUsed += count;
        return {this->Mapped(offset, count), offset};
    }

    Pair<T*, uint32_t> Allocate(uint32_t count, uint32_t alignment = 1u)
    {
        BeginUpdate();
        return AllocateMore(count, alignment);
    }

    void Reset()
    {
        // Called only after device idle by GPUScene::Reset.
        mCurrent.clear();
        mRetired.clear();
        mUsed = 0;
        this->Clear();
    }

    [[nodiscard]] auto GetStatistics() const
    {
        auto stats = PagedBuffer<T, PageShift>::GetStatistics();
        size_t const currentBytes = static_cast<size_t>(mUsed) * sizeof(T);
        stats.retiredBytes = stats.liveBytes - currentBytes;
        stats.liveBytes = currentBytes;
        stats.retiredAllocations = mRetired.size();
        // Do not poll queues or reclaim here: inspecting the profiler must not change residency.
        return stats;
    }

    [[nodiscard]] uint32_t Used() const { return mUsed; }
};
} // namespace GPUSceneBuffers
