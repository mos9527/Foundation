#pragma once
#include <Bits/Format.hpp>
#include <Core/Pool.hpp>
#include "StagingBuffer.hpp"
#include "UploadContext.hpp"
#include "VirtualAllocator.hpp"
namespace Foundation::Rendering
{
    using namespace Foundation;
    using namespace RenderCore;
    using namespace Bits;
    struct GPUSceneBudgets
    {
        // Instance: Full update every frame, fixed allocation sizes
        size_t instanceBudget = 2_MB;
        size_t instanceAlignment = 16_B;
        // Shared: Partial update every frame, dynamic allocation
        size_t sharedBudget = 1_MB;
        size_t sharedStaging = 1_MB;
        // Const: Partial update every frame, dynamic allocation
        size_t constBudget = 128_MB;
        size_t constStaging = 16_MB;
        [[nodiscard]] constexpr size_t totalBudget() const { return instanceBudget + sharedBudget + constBudget; }
        [[nodiscard]] constexpr size_t totalStaging() const { return instanceBudget + sharedStaging; }
    };
    /**
     * @brief Internal data structure for staged data updates.
     */
    struct StagedDoubleBuffer
    {
        Allocator* const alloc;
        char* const data;
        const size_t size;
        const size_t alignment;

        StagedBuffer buffer;
        Async::Mutex mutex;

        StagedDoubleBuffer(RHIDevice* device, size_t size, size_t stagingSize, size_t alignment, Allocator* alloc);
        ~StagedDoubleBuffer();

        template <typename T>
        Span<T> View()
        {
            CHECK_MSG(alignment % alignof(T) == 0, "Bad alignment for type. Type must be aligned on multiples of {}",
                      alignment);
            return {reinterpret_cast<T*>(data), size / sizeof(T)};
        }
    };
    /**
     * @brief Scene data management for asynchronous data updates/uploads on the GPU
     *
     * This implements a 3-tier buffer structure with:
     * - Instance Buffer [e.g. for instances, updated most frequently]
     * - Shared Buffer [e.g. for materials, geometry metadata, updated at different rates]
     * - Const Buffer [e.g. for geometry data (index,vertex,etc.), updated at different rates]
     *
     * The names are merely a hint to how you _could_ update the data at rates that the names imply
     * (i.e. Instance > Shared > Const), though there's no limit, or inherently how these are transferred
     * to the GPU. However, it's still recommended to follow such patterns.
     *
     * The update order is well-defined through @ref CreateUpdatePasses You can expect:
     * - Const, Shared to be updated before Instance [regardless of Push/Update call order]
     * - Instance to be updated after they have been updated [same as above]
     *
     * Separation of buffers is done to reduce the amount of unnecessary barriers on buffers from e.g.
     * to barrier the geometry data buffer even though we'd only touch bytes of instance data - if
     * they're in the same unit.
     *
     * @note All updates are asynchronous - where incremental updates are deferred until GPU execution time,
     * though may still block if the GPU transfer is unavailable
     */
    class GPUScene
    {
        Allocator* mAllocator;
        /* -- Instance -- */
        StagedDoubleBuffer mInstance; // Instance data [updated every frame as a whole]
        bool mInstanceDirty{false};
        Async::Mutex mInstanceMutex; // Mutex for instance data mapping
        /* -- Shared -- */
        StagedDoubleBuffer mShared; // Shared (instance) data [partially updated every frame, could be empty]
        VirtualAllocator mSharedAlloc; // Allocator for Shared data
        Vector<Pair<size_t, size_t>> mSharedUpdateRegions; // Shared regions to update next frame
        /* -- Const -- */
        StagedDoubleBuffer mConst; // Const data [updated on demand, could be empty]
        VirtualAllocator mConstAlloc; // Allocator for Const data
        Vector<Pair<size_t, size_t>> mConstUpdateRegions; // Const regions to update next frame
    public:
        GPUScene(RHIDevice* device, GPUSceneBudgets const& budgets, Allocator* alloc);

        /**
         * @brief Creates a pass that performs per-frame updates with correct synchronization.
         */
        void CreateUpdatePasses(Renderer* renderer, ResourceHandle& outInstanceBuffer, ResourceHandle& outSharedBuffer,
                                ResourceHandle& outConstBuffer,
                                RHIDeviceQueueType queue = RHIDeviceQueueType::Graphics);
        /* -- Instance -- */
        /**
         * @brief Maps the instance data for writing.
         * The returned span is valid until @ref UnmapInstanceData is called.
         *
         * The entirety of the buffer will be updated to the GPU.
         *
         * @note The data update is not immediate, and will be automatically scheduled
         * and performed at the beginning of the next frame.
         *
         * @note This has the side effect of blocking GPU buffer updates,
         * i.e. the @ref CreateUpdatePasses passes, and can thus be used to batch
         * updates and synchronize with the Renderer.
         *
         * @note This locks the internal mutex, and thus is not reentrant.
         */
        template <typename T>
        Span<T> MapInstanceData()
        {
            mInstanceMutex.lock();
            return mInstance.View<T>();
        }
        /**
         * @brief Unmaps the instance data, allowing other threads to map it again.
         *
         * @note The update is guaranteed to be completed by the next frame.
         */
        void UnmapInstanceData()
        {
            mInstanceMutex.unlock();
            mInstanceDirty = true;
        }
        /* -- Shared -- */
        /**
         * @brief Push a block of data into the Shared buffer.
         *
         * The data can be anything from something like an array of matrices, to a custom
         * structure that contains material information, AABB and/or offsets from @ref QueryConst etc.
         * that's expected to be updated at a _different_ (not necessarily lower)
         * granularity.
         *
         * @note The data update is not immediate, and will be automatically scheduled
         * and performed at the beginning of the next frame.
         *
         * @note However, it's valid to @ref QueryShared the allocation right after this call.
         * 
         * @note To batch updates, you can acquire @ref MapInstanceData before pushing,
         * and use @ref UnmapInstanceData afterward to flush all updates at once.
         *
         * @param data Data to push
         * @param alignment Alignment requirement for the data. Must be a multiple of 4.
         * @return Allocation handle. Use @ref QueryShared to get offset/size, and @ref FreeShared to free it.
         */
        VirtualAllocation PushShared(Span<const char> data, size_t alignment);
        /**
         * Queries the Shared allocation for a given allocation.
         * @param allocation Previously allocated Shared allocation
         * @return [Raw offset in bytes, Size in bytes]
         */
        Pair<size_t, size_t> QueryShared(VirtualAllocation allocation);
        /**
         * @brief Updates a previously allocated Shared allocation.
         *
         * @note The update is guaranteed to be completed by the next frame.
         *
         * @note To batch updates, you can acquire @ref MapInstanceData before pushing,
         * and use @ref UnmapInstanceData afterward to flush all updates at once.
         *
         * @param allocation Previously allocated shared allocation
         * @param data New data to write. Must be the same size as the original allocation.
         */
        void UpdateShared(VirtualAllocation allocation, Span<const char> data);
        /**
         * @brief Frees a previously allocated Shared allocation.
         * @param allocation Previously allocated Shared allocation
         */
        void FreeShared(VirtualAllocation allocation);
        /* -- Const -- */
        /**
         * @brief Push a block of data into the Const buffer.
         *
         * The data can be anything from plain old vertex-index buffers, meshlet data
         * and/or any data that's expected to be updated at a _different_ (not necessarily lower)
         * granularity.
         *
         * @note The data update is not immediate, and will be automatically scheduled
         * and performed at the beginning of the next frame.
         * 
         * @note However, it's valid to @ref QueryConst the allocation right after this call.
         *
         * @note To batch updates, you can acquire @ref MapInstanceData before pushing,
         * and use @ref UnmapInstanceData afterward to flush all updates at once.
         *
         * @param data Data to push
         * @param alignment Alignment requirement for the data. Must be a multiple of 4.
         * @return Allocation handle. Use @ref QueryConst to get offset/size, and @ref FreeConst to free it.
         */
        VirtualAllocation PushConst(Span<const char> data, size_t alignment);
        /**
         * @brief Query a previous const allocation
         * @param id Previously allocated const handle
         * @return [Raw offset in bytes, Size in bytes]
         */
        Pair<size_t, size_t> QueryConst(VirtualAllocation id);
        /**
         * @brief Updates a previously allocated Const allocation.
         *
         * @note The data update is not immediate, and will be automatically scheduled
         * and performed at the beginning of the next frame.
         *
         * @note To batch updates, you can acquire @ref MapInstanceData before pushing,
         * and use @ref UnmapInstanceData afterward to flush all updates at once.
         *
         * @param allocation Previously allocated const allocation
         * @param data New data to write. Must be the same size as the original allocation.
         */
        void UpdateConst(VirtualAllocation allocation, Span<const char> data);
        /**
         * @brief Frees a previously allocated const.
         * @param allocation Previously allocated const handle
         */
        void FreeConst(VirtualAllocation allocation);
    };
} // namespace Foundation::Rendering
