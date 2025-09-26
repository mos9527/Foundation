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
        size_t InstanceBudget = 2_MB;
        size_t InstanceStaging = 2_MB;
        size_t InstanceAlignment = 16_B;
        size_t MetadataBudget = 1_MB;
        size_t MetadataStaging = 1_MB;
        size_t PrimitiveBudget = 4_MB;
        size_t PrimitiveStaging = 1_MB;
        size_t VertexBudget = 128_MB;
        size_t VertexStaging = 16_MB;
        size_t IndexBudget = 128_MB;
        size_t IndexStaging = 16_MB;
        constexpr size_t TotalBudget() const { return InstanceBudget + PrimitiveBudget + VertexBudget + IndexBudget; }
    };
    struct SceneMesh
    {
        VirtualAllocation primitive;
        VirtualAllocation vertex;
        VirtualAllocation index;
        // Offsets
        uint32_t primitiveOffset;
        uint32_t vertexOffset;
        uint32_t indexOffset;
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
     * @brief Scene data management for asynchronous data updates/uploads
     */
    class Scene
    {
        Allocator* mAllocator;
        UploadContext mUpload; // Temp upload bump arena
        StagedData mInstance; // Instance data [always updated as a whole, every frame]
        StagedData mMetadata; // Metadata data [partially updated, every frame]
        Vector<Pair<size_t, size_t>> mMetadataRegions; // Metadata regions to update next frame
        /* -- Data -- */
        Pool<SceneHandle, SceneMesh> mMeshes; // All meshes in the scene
        bool mInstanceDirty = false;
        bool mMetadataDirty = false;
    public:
        RHIDeviceScopedObjectHandle<RHIBuffer> mPrimitive, mVertex, mIndex;
        VirtualAllocator mPrimitiveAlloc, mVertexAlloc, mIndexAlloc;
        VirtualAllocator mMetadataAlloc; // Metadata allocator
        Scene(RHIDevice* device, size_t numSwaps, SceneBudgets const& budgets, Allocator* alloc);
        void OnBeforeFrame(uint32_t rendererSync);
        /**
         * @brief Creates a pass that updates the instance buffer with correct synchronization.
         */
        void CreateUpdatePasses(Renderer* renderer, ResourceHandle& outInstanceBuffer, ResourceHandle& outMetadataBuffer,
                                      RHIDeviceQueueType queue = RHIDeviceQueueType::Graphics);
        /* -- Meshes -- */
        /**
         * @brief Push a block of data into the Metadata buffer.
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
         * @note The offsets are packed in the @ref mMetadata buffer as a @ref PrimitiveMetadata struct,
         *       which can be accessed by querying the @ref SceneMesh.primitiveMetadata allocation.
         *
         * @param vertices Vertex data of any type
         * @param indices Index data of any type
         * @return Handle to the created mesh. Use @ref QueryMesh to get offsets.
         */
        SceneHandle PushMesh(Span<const char> vertices, Span<const char> indices);
        /**
         * @brief Query a previous Mesh allocation
         * @param id Previously allocated mesh handle
         * @return A @ref SceneMesh containing all allocation handles
         */
        SceneMesh QueryMesh(SceneHandle id);
        void DestroyMesh(SceneHandle mesh);
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
        /* -- Metadata -- */
        /**
         * @brief Push a block of data into the Metadata buffer.
         *
         * The data can be anything from something like an array of matrices, to a custom
         * structure that contains material information, AABB, etc.
         *
         * @note The update is guaranteed to be completed by the next frame.
         *
         * @param data Data to push
         * @param alignment Alignment requirement for the data. Must be a multiple of 4.
         * @return Allocation handle. Use @ref QueryMetadata to get offset/size, and @ref FreeMetadata to free it.
         */
        VirtualAllocation PushMetadata(Span<const char> data, size_t alignment);
        /**
         * Queries the metadata allocation for a given allocation.
         * @param allocation Previously allocated metadata allocation
         * @return [Raw offset in bytes, Size in bytes]
         */
        Pair<size_t, size_t> QueryMetadata(VirtualAllocation allocation);
        /**
         * @brief Updates a previously allocated metadata allocation.
         *
         * @note The update is guaranteed to be completed by the next frame.
         *
         * @param allocation Previously allocated metadata allocation
         * @param data New data to write. Must be the same size as the original allocation.
         */
        void UpdateMetadata(VirtualAllocation allocation, Span<const char> data);
        /**
         * @brief Frees a previously allocated metadata allocation.
         * @param allocation Previously allocated metadata allocation
         */
        void FreeMetadata(VirtualAllocation allocation);
    };
} // namespace ModelViewer
