#pragma once
#include <Rendering/StagingBuffer.hpp>
#include "Mesh.hpp"
namespace Foundation
{
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    using namespace Foundation::Rendering;
    using namespace Foundation::Math;
    #include "Shaders/Common.h"
    // Primitive, Vertex , Index
    using SceneHandle = DataHandle;
    using MeshHandle = Tuple<DataHandle, DataHandle, DataHandle>;
    const MeshHandle kInvalidMeshHandle{ kInvalidHandle, kInvalidHandle, kInvalidHandle };
    inline DataHandle PrimitiveIDOf(MeshHandle const& handle) { return std::get<0>(handle); }
    struct SceneBudgets
    {
        size_t StagingBudget{16 * (1 << 20)};
        size_t InstanceDataBudget{16 * (1 << 20)};
        size_t PrimitiveDataBudget{16 * (1 << 20)};
        size_t VertexDataBudget{16 * (1 << 20)};
        size_t IndexDataBudget{16 * (1 << 20)};
    };
    /**
     * @brief Scene representation for GPU driven rendering.
     *
     * Primitive data, instance data, and global data are represented in flat buffers.
     * See @ref PrimitiveMetadata, @ref InstanceMetadata for data layouts.
     */
    class Scene
    {
    public:
        enum class State
        {
            Idle,
            Transfer,
        };
    private:
        RHIDevice* m_device{nullptr};
        Allocator* m_allocator{nullptr};

        Vector<StagingBuffer> m_stagingBuffers;
        UniquePtr<DataBuffer> m_instanceData, m_primitiveData, m_vertexData, m_indexData;

        ResourceHandle CreateUpdatePass(Renderer* renderer, DataBuffer* data_buffer, StringView name, RHIDeviceQueueType queue);

        State m_state{State::Idle};
        uint32_t m_currentSync{0};
        RHIDeviceIdleGuard m_idleGuard;
    public:
        Scene(RHIDevice* device, Allocator* allocator, uint32_t numSwaps, SceneBudgets const& budgets = {});
        ResourceHandle CreateInstanceDataUpdate(Renderer* renderer, StringView name = "Scene Instance Data Update",
                                                RHIDeviceQueueType queue = RHIDeviceQueueType::Graphics)
        {
            return CreateUpdatePass(renderer, m_instanceData.get(), name, queue);
        }
        ResourceHandle CreatePrimitiveDataUpdate(Renderer* renderer, StringView name = "Scene Primitive Data Update",
                                        RHIDeviceQueueType queue = RHIDeviceQueueType::Graphics)
        {
            return CreateUpdatePass(renderer, m_primitiveData.get(), name, queue);
        }
        ResourceHandle CreateVertexDataUpdate(Renderer* renderer, StringView name = "Scene Vertex Data Update",
                                        RHIDeviceQueueType queue = RHIDeviceQueueType::Graphics)
        {
            return CreateUpdatePass(renderer, m_vertexData.get(), name, queue);
        }
        ResourceHandle CreateIndexDataUpdate(Renderer* renderer, StringView name = "Scene Index Data Update",
                                        RHIDeviceQueueType queue = RHIDeviceQueueType::Graphics)
        {
            return CreateUpdatePass(renderer, m_indexData.get(), name, queue);
        }

        StagingBuffer* GetStagingBuffer();

        /**
         * @brief Resets the staging buffer and aborts all pending data updates.
         */
        void BeginTransfer(uint32_t rendererSync);
        MeshHandle CreateMesh(Mesh const& mesh);
        void FreeMesh(MeshHandle const& handle);

        SceneHandle CreateInstance(InstanceMetadata data);
        void UpdateInstance(SceneHandle instance, InstanceMetadata const& data);
        void FreeInstance(SceneHandle handle);
        /**
         * @brief Ends the transfer state.
         */
        void EndTransfer();
    };
    ENUM_NAME_CONV_BEGIN(Scene::State)
        ENUM_NAME(Idle)
        ENUM_NAME(Transfer)
    ENUM_NAME_CONV_END()
} // namespace Foundation
