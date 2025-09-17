#pragma once
#include "Common.hpp"
#include "Resource.hpp"
#include <RHICore/Command.hpp>
#include <Core/Allocator/StackAllocator.hpp>
namespace Foundation::RHI {
    class VulkanDevice;
    class VulkanCommandList;
    class VulkanCommandPool : public RHICommandPool {
    protected:
        Allocator* m_allocator;
        const VulkanDevice& m_device;
        vk::raii::CommandPool m_commandPool{ nullptr };
        RHIObjectStorage<> m_storage;
        constexpr static size_t kCommandListReserveSize = 256;
    public:
        VulkanCommandPool(const VulkanDevice& device, PoolDesc const& desc, Allocator* allocator);

        auto const& GetDevice() const { return m_device; }
        auto const& GetVkCommandPool() const { return m_commandPool; }

        RHICommandPoolScopedHandle<RHICommandList> CreateCommandList() override;
        RHICommandList* GetCommandList(Handle handle) const override;
        void DestroyCommandList(Handle handle) override;

        void DebugSetObjectName(const char* name) override;
    };
    constexpr size_t kCommandBarrierReserveSize = 256;
    class VulkanCommandList : public RHICommandList {
    protected:
        const VulkanCommandPool& m_commandPool;
        vk::raii::CommandBuffer m_commandBuffer{ nullptr };

        ScopedArena m_arena;
        // Stack allocator for temporary allocations during command list execution
        // Only valid within Begin(), End() clause
        StackAllocator m_allocator;
        constexpr static size_t kArenaSize = 2LL * (1LL << 20); // 2 MB

        struct Barriers {
            Vector<vk::ImageMemoryBarrier2> image;
            Vector<vk::BufferMemoryBarrier2> buffer;
            Barriers(Allocator* allocator) : image(allocator), buffer(allocator)
            {
                image.reserve(kCommandBarrierReserveSize);
                buffer.reserve(kCommandBarrierReserveSize);
            };
        };
        // !! XXX: Resources allocated with m_allocator must be destroyed before
        // the allocator goes down.
        // VTable can in-fact get corrupted otherwise - we need a better solution
        // to safeguard this type of issue.
        UniquePtr<Barriers> m_barriers;
    public:
        VulkanCommandList(const VulkanCommandPool& commandPool);

        auto const& GetVkCommandBuffer() const { return m_commandBuffer; }

        RHICommandList& Begin() override;

        RHICommandList& BeginTransition() override;
        RHICommandList& SetBufferTransition(RHIBuffer* image, TransitionDesc const& desc) override;
        RHICommandList& SetImageTransition(RHITexture* image, TransitionDesc const& desc) override;
        RHICommandList& EndTransition() override;

        RHICommandList& SetPipeline(PipelineDesc const& desc) override;
        RHICommandList& BindDescriptorSet(
            RHIDevicePipelineType bindpoint,
            RHIPipelineState* pipeline,
            Span<RHIDeviceDescriptorSet* const> sets,
            size_t first) override;
        RHICommandList& SetViewport(float x, float y, float width, float height, float depth_min = 0.0, float depth_max = 1.0) override;
        RHICommandList& SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
        RHICommandList& Draw(uint32_t vertex_count, uint32_t instance_count = 1, uint32_t first_vertex = 0, uint32_t first_instance = 0) override;
        RHICommandList& DrawIndexed(uint32_t index_count, uint32_t instance_count = 1, uint32_t first_index = 0, int32_t vertex_offset = 0, uint32_t first_instance = 0) override;
        RHICommandList& DrawIndexedIndirectCount(RHIBuffer* buffer, size_t offset, RHIBuffer* count_buffer, size_t count_offset, uint32_t max_draw_count, uint32_t stride) override;

        RHICommandList& PushConstant(RHIPipelineState* pipeline, RHIShaderStage stage, uint32_t offset, Core::Span<const char> data) override;
        RHICommandList& FillBuffer(RHIBuffer* buffer, uint32_t value, size_t offset = 0, size_t size = kFullSize) override;
        RHICommandList& CopyBuffer(RHIBuffer* src_buffer, RHIBuffer* dst_buffer, Core::Span<const CopyBufferRegion> regions) override;
        RHICommandList& CopyImage(RHITexture* src_image, RHITextureLayout src_layout, RHITexture* dst_image, RHITextureLayout dst_layout, Core::Span<const CopyImageRegion> regions) override;
        RHICommandList& CopyBufferToImage(RHIBuffer* src_buffer, RHITexture* dst_image, RHITextureLayout dst_layout, Core::Span<const CopyImageRegion> regions) override;

        RHICommandList& BeginGraphics(GraphicsDesc const& desc) override;
        RHICommandList& BindVertexBuffer(uint32_t index, Core::Span<RHIBuffer* const> buffers, Core::Span<const size_t> offsets) override;
        RHICommandList& BindIndexBuffer(RHIBuffer* buffer, size_t offset, RHIResourceFormat format) override;
        RHICommandList& EndGraphics() override;

        RHICommandList& Dispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) override;

        RHICommandList& DebugBegin(const char* message) override;
        RHICommandList& DebugInsertMarker(const char* message) override;
        RHICommandList& DebugEnd() override;

        void End() override;
        void Reset() override;

        void DebugSetObjectName(const char* name) override;
    };
}
