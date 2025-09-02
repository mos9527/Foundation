#pragma once
#include <RHICore/Resource.hpp>
#include <RHICore/Device.hpp>
#include <Core/Allocator/Allocator.hpp>
#include <Core/Container/FreeList.hpp>
namespace Foundation {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    class Renderer;    
    using SceneHandle = size_t;
    // When reallocating internal buffers, grow by this factor
    constexpr float kReallocGrowthFactor = 2.0f;
    struct SceneDataDesc {
        size_t PrimitiveDataBudget{ 256 * (1 << 20LL) };
        size_t InstanceDataBudget{ 64 * (1 << 20LL) }; 
        size_t GlobalDataBudget{ 1 * (1 << 20LL) };

        size_t TotalBudget() const { return PrimitiveDataBudget + InstanceDataBudget + GlobalDataBudget; }
    };
    /* Data representation for GPU driven rendering.

    SceneData merely consists flat, opaque representations of:
        - Primitive data (meshes, TLAS etc.)
        - Texture maps (2D, Cubemaps, etc.)
        - Instance data
            - Instanced draws
            - Geo transforms, materials
            - GPU skinning matrices, etc
        - Global data
            - Camera info, lighting info, etc.

    * Eventual draw calls, etc, are only issued by the GPU via RenderPass that consumes these data.
    * Therefore, the SceneData class may have no knowledge of the underlying data structures or how they are used.

    NOTES:
        API implementations wise, descriptor indexing has been the most available one yet
            - https://docs.vulkan.org/samples/latest/samples/extensions/descriptor_indexing/README.html

        There's also a Vulkan-only extension that allows using buffer addresses w/ GPU pointers directly
        Not considered for now.
            - https://docs.vulkan.org/samples/latest/samples/extensions/buffer_device_address/README.html
    */
    class SceneData {
    private:
        using AllocationList = FreeList<SceneHandle, RHIBuffer::Arena::Allocation>;
        Allocator* m_allocator{ nullptr };
        Renderer& m_renderer;

        // GPU Staging data
        RHIDeviceScopedObjectHandle<RHIBuffer> m_stagingBuffer;
        // CPU Staging data
        StlVector<uint8_t> m_staging;

        // Primitive data (e.g. vertex buffers, index buffers, etc.)
        // compacted into a fixed max-size homogeneous array
        // Expect this to be stored in GPU local memory.
        RHIDeviceScopedObjectHandle<RHIBuffer>
            m_primitiveData;        
        AllocationList m_primitives;
        // Textures used in the scene (2D, Cubemaps, etc.)
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
        using StagingList = StlVector<RHI::RHICommandList::CopyBufferRegion>;
        // Staging offset, Allocation handle
        StagingList m_primitiveStaging, m_instanceStaging;

        // Push data to the staging buffer, and queue for GPU transfer
        RHIBuffer::Arena::Allocation PushData(RHIBuffer* buffer, StagingList& staging, StlSpan<const uint8_t> data, size_t alignment = 16);
        // Update previously allocated data
        void UpdateData(RHIBuffer* buffer, StagingList& staging, RHIBuffer::Arena::Allocation handle, StlSpan<const uint8_t> data);
        // Free previously allocated data
        // This is a no-op GPU-wise, and only frees CPU tracked allocations
        void FreeData(RHIBuffer* buffer, RHIBuffer::Arena::Allocation alloc);

        // Commit all staged data to GPU buffers
        // Temporary staging buffers are used to batch all data to be transferred
        // * No GPU resources are allocated until EndTransfer is called
        // * No actual transfer is committed until Update is called.
        void EndTransfer(RHICommandList* cmd);

        const SceneDataDesc m_desc;
    public:
        SceneData(Allocator* allocator, Renderer& renderer, SceneDataDesc const& desc);

        SceneHandle AddPrimitiveData(StlSpan<const uint8_t> data, size_t alignment = 16);
        void UpdatePrimitiveData(SceneHandle handle, StlSpan<const uint8_t> data);
        void FreePrimitiveData(SceneHandle handle);
        std::pair<size_t, size_t> QueryPrimitiveDataSizeAndOffset(SceneHandle handle) const;

        SceneHandle AddInstanceData(StlSpan<const uint8_t> data, size_t alignment = 16);
        void UpdateInstanceData(SceneHandle handle, StlSpan<const uint8_t> data);
        void FreeInstanceData(SceneHandle handle);
        std::pair<size_t, size_t> QueryInstanceDataSizeAndOffset(SceneHandle handle) const;

        RHIDeviceObjectHandle<RHIBuffer> GetPrimitiveDataBuffer() const { return m_primitiveData; }
        RHIDeviceObjectHandle<RHIBuffer> GetInstanceDataBuffer() const { return m_instanceData; }
        RHIDeviceObjectHandle<RHIBuffer> GetGlobalDataBuffer() const { return m_globalData; }

        /// <summary>
        /// Update GPU buffers with the latest data
        /// This may involve resource synchronization and copying, unless no transfer is needed.
        /// In which case, this is a no-op. 
        /// </summary>
        /// <param name="cmd">Command list to be potentially populated.</param>
        /// <returns>true if command list is populated. Otherwise false.</returns>
        bool Update(RHICommandList* cmd);
        // Cancel all pending staged transfers
        void Abort();
        // Clear all resources
        void Reset();
    };
}
