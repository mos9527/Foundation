#pragma once
#include <RHICore/Resource.hpp>
#include <Core/Allocator/Allocator.hpp>
#include <Core/Container/FreeList.hpp>
#include <Rendering/Renderer.hpp>
namespace Foundation {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;    
    using SceneHandle = uint32_t;
    struct SceneDataDesc {
        size_t PrimitiveDataBudget{ 32 * (1 << 20LL) };
        size_t InstanceDataBudget{ 32 * (1 << 20LL) };
        size_t VertexDataBudget{ 32 * (1 << 20LL) };
        size_t IndexDataBudget{ 32 * (1 << 20LL) };

        [[nodiscard]] size_t TotalBudget() const
        {
            return PrimitiveDataBudget + InstanceDataBudget +
                VertexDataBudget + IndexDataBudget;
        }
    };
    /**
     * @brief Flat buffers for GPU driven rendering
     */
    class SceneData {
        using AllocationList = FreeList<SceneHandle, RHIBuffer::Arena::Allocation>;
        Allocator* m_allocator{ nullptr };

        // GPU Staging data
        RHIDeviceScopedObjectHandle<RHIBuffer> m_stagingBuffer;
        // CPU Staging data
        Vector<char> m_staging;

        // Primitive data metadata
        // compacted into a fixed max-size homogeneous array
        // Expect this to be stored in GPU local memory.
        RHIDeviceScopedObjectHandle<RHIBuffer>
            m_primitiveData;        
        AllocationList m_primitives;
        // Vertex buffer data
        // Expect this to be stored in GPU local memory.
        RHIDeviceScopedObjectHandle<RHIBuffer>
            m_vertexData;
        AllocationList m_vertices;
        // Index buffer data
        // Expect this to be stored in GPU local memory.
        RHIDeviceScopedObjectHandle<RHIBuffer>
            m_indexData;
        AllocationList m_indices;
        // Textures used in the scene (2D, Cube maps, etc.)
        // Expect this to be stored in GPU local memory.
        FreeList<SceneHandle, RHIDeviceScopedObjectHandle<RHITexture>>
            m_textures;
        // Instance data (e.g. instance transforms, material indices, etc.)
        // Expect this to be stored in GPU local memory.
        RHIDeviceScopedObjectHandle<RHIBuffer>
            m_instanceData;        
        AllocationList m_instances;
        // Global data (e.g. camera info, lighting info, etc.)
        // Expect this to be dynamic - and stored in host visible memory.
        RHIDeviceScopedObjectHandle<RHIBuffer>
            m_globalData;
        using StagingList = Vector<RHI::RHICommandList::CopyBufferRegion>;
        // Staging offset, Allocation handle
        StagingList m_primitiveStaging, m_instanceStaging, m_vertexStaging, m_indexStaging;
        // Temporary staging list used for coalescing regions
        StagingList m_colescedTempStaging;
        // Coalesce regions in the staging list to minimize number of copies
        StagingList& CoalesceStaging(StagingList& list);
        // Push data to the staging buffer, and queue for GPU transfer
        RHIBuffer::Arena::Allocation PushData(RHIBuffer* buffer, StagingList& staging, Span<const char> data, size_t alignment = 16);
        // Update previously allocated data
        void UpdateData(RHIBuffer* buffer, StagingList& staging, RHIBuffer::Arena::Allocation handle, Span<const char> data);
        // Free previously allocated data
        // This is a no-op GPU-wise, and only frees CPU tracked allocations
        static void FreeData(RHIBuffer* buffer, RHIBuffer::Arena::Allocation alloc);

        // Commit all staged data to GPU buffers
        // Temporary staging buffers are used to batch all data to be transferred
        // * No GPU resources are allocated until EndTransfer is called
        // * No actual transfer is committed until Update is called.
        void EndTransfer(RHICommandList* cmd);

        const SceneDataDesc m_desc;

        SceneHandle AddData(Span<const char> data, RHIBuffer* buffer, AllocationList& alloc, StagingList& staging, size_t alignment = 16);
        void UpdateData(SceneHandle handle, Span<const char> data, RHIBuffer* buffer, AllocationList& alloc, StagingList& staging);
        void FreeData(SceneHandle handle, RHIBuffer* buffer, AllocationList& alloc);
        [[nodiscard]] Pair<size_t, size_t> QueryDataSizeAndOffset(SceneHandle handle, RHIBuffer* buffer, AllocationList const& alloc) const;

        bool m_initialized{ false };
        RHIDeviceIdleGuard m_waitIdle;
    public:
        SceneData(Allocator* allocator, RHIDevice* device, SceneDataDesc const& desc);

        SceneHandle AddPrimitiveData(Span<const char> data, size_t alignment = 16)
        { return AddData(data, m_primitiveData.Get(), m_primitives, m_primitiveStaging, alignment); }
        void UpdatePrimitiveData(SceneHandle handle, Span<const char> data)
        { UpdateData(handle, data, m_primitiveData.Get(), m_primitives, m_primitiveStaging); }
        void FreePrimitiveData(SceneHandle handle)
        { FreeData(handle, m_primitiveData.Get(), m_primitives); }
        [[nodiscard]] Pair<size_t, size_t> QueryPrimitiveDataSizeAndOffset(SceneHandle handle) const
        { return QueryDataSizeAndOffset(handle, m_primitiveData.Get(), m_primitives); }
        [[nodiscard]] RHIDeviceObjectHandle<RHIBuffer> GetPrimitiveDataBuffer() const { return m_primitiveData; }

        SceneHandle AddInstanceData(Span<const char> data, size_t alignment = 16)
        { return AddData(data, m_instanceData.Get(), m_instances, m_instanceStaging, alignment); }
        void UpdateInstanceData(SceneHandle handle, Span<const char> data)
        { UpdateData(handle, data, m_instanceData.Get(), m_instances, m_instanceStaging); }
        void FreeInstanceData(SceneHandle handle)
        { FreeData(handle, m_instanceData.Get(), m_instances); }
        [[nodiscard]] Pair<size_t, size_t> QueryInstanceDataSizeAndOffset(SceneHandle handle) const
        { return QueryDataSizeAndOffset(handle, m_instanceData.Get(), m_instances); }
        [[nodiscard]] RHIDeviceObjectHandle<RHIBuffer> GetInstanceDataBuffer() const { return m_instanceData; }

        SceneHandle AddVertexData(Span<const char> data, size_t alignment = 16)
        { return AddData(data, m_vertexData.Get(), m_vertices, m_vertexStaging, alignment); }
        void UpdateVertexData(SceneHandle handle, Span<const char> data)
        { UpdateData(handle, data, m_vertexData.Get(), m_vertices, m_vertexStaging); }
        void FreeVertexData(SceneHandle handle)
        { FreeData(handle, m_vertexData.Get(), m_vertices); }
        [[nodiscard]] Pair<size_t, size_t> QueryVertexDataSizeAndOffset(SceneHandle handle) const
        { return QueryDataSizeAndOffset(handle, m_vertexData.Get(), m_vertices); }
        [[nodiscard]] RHIDeviceObjectHandle<RHIBuffer> GetVertexDataBuffer() const { return m_vertexData; }

        SceneHandle AddIndexData(Span<const char> data, size_t alignment = 16)
        { return AddData(data, m_indexData.Get(), m_indices, m_indexStaging, alignment); }
        void UpdateIndexData(SceneHandle handle, Span<const char> data)
        { UpdateData(handle, data, m_indexData.Get(), m_indices, m_indexStaging); }
        void FreeIndexData(SceneHandle handle)
        { FreeData(handle, m_indexData.Get(), m_indices); }
        [[nodiscard]] Pair<size_t, size_t> QueryIndexDataSizeAndOffset(SceneHandle handle) const
        { return QueryDataSizeAndOffset(handle, m_indexData.Get(), m_indices); }
        [[nodiscard]] RHIDeviceObjectHandle<RHIBuffer> GetIndexDataBuffer() const { return m_indexData; }

        [[nodiscard]] RHIDeviceObjectHandle<RHIBuffer> GetGlobalDataBuffer() const { return m_indexData; }

        bool HasUpdates() const
        {
            return !m_initialized || !m_staging.empty();
        }
        /**
         * @brief Update GPU buffers with the latest data
         * This may involve resource synchronization and copying, unless no transfer is needed.
         * In which case, this is a no-op. 
         */
        /// <param name="cmd">Command list to be potentially populated.</param>
        /// <returns>true if command list is populated. Otherwise, false.</returns>
        void Update(RHICommandList* cmd);
        // Cancel all pending staged transfers
        void Abort();
        // Clear all resources
        void Reset();
    };
}
