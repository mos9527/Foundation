#pragma once
#include <Async/Future.hpp>
#include <Atomics/Queue.hpp>
#include <Bits/Format.hpp>
#include <Core/Pool.hpp>
#include <Math/Math.hpp>
#include <Rendering/StagingBuffer.hpp>
#include <Rendering/UploadContext.hpp>
#include <Rendering/VirtualAllocator.hpp>
namespace ModelViewer
{
    using namespace Foundation;
    using namespace RenderCore;
    using namespace Rendering;
    using namespace Math;
    using namespace Bits;
#include "Shaders/Common.h"
    using SceneHandle = uint32_t;
    struct SceneBudgets
    {
        // Instance: Full update every frame, fixed allocation sizes
        size_t instanceBudget = 2_MB;
        size_t instanceAlignment = 16_B;
        // Shared: Partial update every frame, dynamic allocation
        size_t sharedBudget = 1_MB;
        size_t sharedStaging = 1_MB;
        // Geometry: Updates on-demand, dynamic allocation
        size_t geometryBudget = 128_MB;
        size_t geometryStaging = 16_MB;
        [[nodiscard]] constexpr size_t totalBudget() const { return instanceBudget + sharedBudget + geometryBudget; }
        [[nodiscard]] constexpr size_t totalStaging() const { return instanceBudget + sharedStaging; }
    };

    struct StagedData
    {
        Allocator* const alloc;
        char* const data;
        const size_t size;
        const size_t alignment;

        StagedBuffer buffer;
        Async::Mutex mutex;

        StagedData(RHIDevice* device, size_t size, size_t alignment, size_t numSwaps, Allocator* alloc);
        ~StagedData();

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
     */
    class GPUScene
    {
        Allocator* mAllocator;
        UploadContext mUpload; // Temp upload bump arena of @ref totalStaging budget
        /* -- Instance -- */
        StagedData mInstance; // Instance data [updated every frame as a whole]
        bool mInstanceDirty{true};
        /* -- Shared -- */
        StagedData mShared; // Shared (instance) data [partially updated every frame, could be empty]
        VirtualAllocator mSharedAlloc; // Allocator for Shared data
        Vector<Pair<size_t, size_t>> mSharedUpdateRegions; // Shared regions to update next frame
        bool mSharedDirty{true};
        /* -- Geometry -- */
        RHIDeviceScopedObjectHandle<RHIBuffer> mGeometry; // Geometry data [updated on demand, blocking]
        VirtualAllocator mGeometryAlloc; // Allocator for Geometry data
    public:
        GPUScene(RHIDevice* device, size_t numSwaps, SceneBudgets const& budgets, Allocator* alloc);

        /**
         * @brief Creates a pass that performs per-frame updates with correct synchronization.
         */
        void CreateUpdatePasses(Renderer* renderer, ResourceHandle& outInstanceBuffer, ResourceHandle& outSharedBuffer,
                                ResourceHandle& outGeometryBuffer,
                                RHIDeviceQueueType queue = RHIDeviceQueueType::Graphics);

        /* -- Instance -- */
        /**
         * @brief Maps the instance data for writing.
         * The returned span is valid until @ref UnmapInstanceData is called.
         *
         * The entirety of the buffer will be updated to the GPU.
         *
         * @note The backing memory is CPU-only, and is NOT mapped to the GPU memory
         * nor the driver. RW is always possible.
         *
         * @note There's NO atomic guarantees on the backing data.
         *
         * @note The data update is not immediate, and will be automatically scheduled
         * and performed at the beginning of the next frame.
         *
         * @note This locks the internal mutex, and thus is not reentrant.
         */
        template <typename T>
        Span<T> MapInstanceData()
        {
            mInstance.mutex.lock();
            return mInstance.View<T>();
        }
        /**
         * @brief Unmaps the instance data, allowing other threads to map it again.
         *
         * @note The update is guaranteed to be completed by the next frame.
         */
        void UnmapInstanceData()
        {
            mInstance.mutex.unlock();
            mInstanceDirty = true;
        }
        /* -- Shared -- */
        /**
         * @brief Push a block of data into the Shared buffer.
         *
         * The data can be anything from something like an array of matrices, to a custom
         * structure that contains material information, AABB, etc.
         *
         * @note The update is guaranteed to be completed by the next frame.
         *
         * @param data Data to push
         * @param alignment Alignment requirement for the data. Must be a multiple of 4.
         * @return Allocation handle. Use @ref QueryShared to get offset/size, and @ref FreeShared to free it.
         */
        VirtualAllocation PushShared(Span<const char> data, size_t alignment);
        /**
         * Queries the shared allocation for a given allocation.
         * @param allocation Previously allocated shared allocation
         * @return [Raw offset in bytes, Size in bytes]
         */
        Pair<size_t, size_t> QueryShared(VirtualAllocation allocation);
        /**
         * @brief Updates a previously allocated shared allocation.
         *
         * @note The update is guaranteed to be completed by the next frame.
         *
         * @param allocation Previously allocated shared allocation
         * @param data New data to write. Must be the same size as the original allocation.
         */
        void UpdateShared(VirtualAllocation allocation, Span<const char> data);
        /**
         * @brief Frees a previously allocated shared allocation.
         * @param allocation Previously allocated shared allocation
         */
        void FreeShared(VirtualAllocation allocation);
        /* -- Geometries -- */
        /**
         * @brief Push a block of data into the Shared buffer.
         *
         * @note This is a blocking operation, and will stall until the staging buffer is available
         * from the GPU.
         *
         * @note The update is guaranteed to be completed by the next frame.
         *
         * @note There's no requirement for the data to be alive after this call as it's copied
         *       into the staging buffer immediately.
         *       Uploads are guaranteed to be completed by the next frame.
         *
         * @note The offsets are packed in the @ref mShared buffer as a @ref PrimitiveShared struct,
         *       which can be accessed by querying the @ref Scenegeometry.primitiveShared allocation.
         *
         * @param data Raw geometry data
         * @return Handle to the created geometry. Use @ref QueryGeometry to get offsets.
         */
        VirtualAllocation PushGeometry(Span<const char> data, size_t alignment);
        /**
         * @brief Query a previous geometry allocation
         * @param id Previously allocated geometry handle
         * @return A @ref Scenegeometry containing all allocation handles
         */
        Pair<size_t, size_t> QueryGeometry(SceneHandle id);
        /**
         * @brief Frees a previously allocated geometry.
         * @param geometry Previously allocated geometry handle
         */
        void DestroyGeometry(SceneHandle geometry);
    };
} // namespace ModelViewer
