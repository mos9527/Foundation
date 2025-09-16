#pragma once
#include <Rendering/StagingBuffer.hpp>
#include <Bits/Format.hpp>
#include <Async/Future.hpp>
#include <Math/Math.hpp>
using namespace Foundation::Math;
#include "Shaders/Common.h"
namespace Foundation
{
    using namespace Core;
    using namespace RHI;
    using namespace Rendering;
    using namespace Native;
    using namespace Async;
    using SceneHandle = uint32_t;
    struct SceneBudgets
    {
        size_t InstanceBudget = 16_MB;
        size_t InstanceStaging = 1_MB;
        size_t PrimitiveBudget = 16_MB;
        size_t PrimitiveStaging = 1_MB;
        size_t VertexBudget = 128_MB;
        size_t VertexStaging = 16_MB;
        size_t IndexBudget = 128_MB;
        size_t IndexStaging = 16_MB;
    };
    struct MeshAllocation
    {
        uint32_t primitiveID;
        BufferAllocation vertex;
        BufferAllocation index;
    };
    class Scene;
    class SceneFuture : public Future<SceneHandle>
    {
        const SharedPtr<Mutex> mutex;
        const void* data;
    public:
        SceneFuture(const SharedPtr<Mutex>& mutex, void* data) : mutex(mutex), data(data) {}
        /**
         * @brief Wait for the future to complete.
         * @note This _must_ be called on a different thread than the Scene one, otherwise a deadlock is guaranteed.
         */
        void wait() const;
        template<typename T> T* get() { wait(); return static_cast<T*>(data); }
    };
    /**
     * @brief Scene data management for asynchronous data updates/uploads
     */
    class Scene
    {
    public:
        enum class State
        {
            Idle,
            Update,
            UpdateAsync
        };
    private:
        Allocator* m_allocator;
        StagedBuffer m_instanceBuffer;
        StagedBuffer m_primitiveBuffer;
        StagedBuffer m_vertexBuffer, m_indexBuffer;
        /**
         * @brief Resets all staging buffers and aborts all pending data updates.
         *
         * @note This MUST be called after the @ref Renderer's @ref BeginExecute,
         * and before @ref ExecuteFrame as the updates are tied to the frame fences.
         */
        void BeginUpdate(uint32_t rendererSync);
        void EndUpdate();
        State m_state{ State::Idle };
        // [Instance ID, Data]
        Queue<Pair<SceneHandle, InstanceMetadata>> m_instanceQueue;
        // [Signal Mutex, Path, Allocation Ptr]
        Queue<Tuple<SharedPtr<Mutex>, Path, MeshAllocation*>> m_meshQueue;
        List<MeshAllocation> m_meshes;
        Mutex m_updateMutex;
    public:
        void CreateUpdatePasses(Renderer* renderer,
            ResourceHandle& outInstance, ResourceHandle& outPrimitive,
            ResourceHandle& outVertex, ResourceHandle& outIndex,
            RHIDeviceQueueType queue = RHIDeviceQueueType::Graphics
        );
        Scene(RHIDevice* device, Allocator* alloc, size_t numSwaps, SceneBudgets const& budgets = {});

        void OnBeforeFrame(uint32_t rendererSync);

        void BeginUpdateAsync();
        SceneFuture LoadMeshAsync(Path path);
        void UpdateInstanceAsync(SceneHandle id, InstanceMetadata data);
        void EndUpdateAsync();
    };
    ENUM_NAME_CONV_BEGIN(Scene::State)
    ENUM_NAME(Idle)
    ENUM_NAME(Update)
    ENUM_NAME(UpdateAsync)
    ENUM_NAME_CONV_END()
}