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
        size_t InstanceBudget = 4_MB;
        size_t InstanceStaging = 4_MB;
        size_t InstanceAlignment = 16_B;
        size_t PrimitiveBudget = 4_MB;
        size_t PrimitiveStaging = 1_MB;
        size_t VertexBudget = 128_MB;
        size_t VertexStaging = 16_MB;
        size_t IndexBudget = 128_MB;
        size_t IndexStaging = 16_MB;
        constexpr size_t TotalBudget() const
        {
            return InstanceBudget + PrimitiveBudget + VertexBudget + IndexBudget;
        }
    };
    struct SceneMesh
    {
        uint32_t primitiveID;
        VirtualAllocation vertex;
        VirtualAllocation index;
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

        template<typename T> Span<T> View()
        {
            CHECK_MSG(alignment % alignof(T) == 0, "Bad alignment for type. Type must be aligned on multiples of {}", alignment);
            return { reinterpret_cast<T*>(data), size / sizeof(T) };
        }
    };
    /**
     * @brief Scene data management for asynchronous data updates/uploads
     */
    class Scene
    {
        Allocator* m_allocator;
        UploadContext m_upload; // Temp upload bump arena
        StagedData m_instance; // Instance data with per-swap staging
        /* -- Data -- */
        Pool<SceneHandle, SceneMesh> m_meshes; // All meshes in the scene
        bool m_instanceDirty = false;
    public:
        RHIDeviceScopedObjectHandle<RHIBuffer>
            m_prmitive, m_vertex, m_index; // GPU Buffers
        VirtualAllocator
            m_prmitiveAlloc, m_vertexAlloc, m_indexAlloc; // GPU virtual allocator
        Scene(RHIDevice* device,  size_t numSwaps, SceneBudgets const& budgets, Allocator* alloc);
        void OnBeforeFrame(uint32_t rendererSync);

        /* -- Meshes -- */
        SceneHandle CreateMesh(Span<const char> vertices, Span<const char> indices);
        SceneMesh GetMesh(SceneHandle id);
        void DestroyMesh(SceneHandle mesh);

        /**
         * @brief Creates a pass that updates the instance buffer with correct synchronization.
         */
        void CreateInstanceUpdatePass(Renderer* renderer, ResourceHandle& outInstanceBuffer, RHIDeviceQueueType queue = RHIDeviceQueueType::Graphics);
        /**
         * @brief Maps the instance data for writing.
         * The returned span is valid until @ref UnmapInstanceData is called.
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
        template<typename T> Span<T> MapInstanceData()
        {
            m_instance.mutex.lock();
            return m_instance.View<T>();
        }
        /**
         * @brief Unmaps the instance data, allowing other threads to map it again.
         */
        void UnmapInstanceData()
        {
            m_instance.mutex.unlock();
            m_instanceDirty = true;
        }
    };
}