#pragma once
#include <Platform/RHI/Command.hpp>
#include <Platform/RHI/Vulkan/Common.hpp>
#include <Platform/RHI/Vulkan/Resource.hpp>

#include <Core/Allocator/StlContainers.hpp>
#include <Core/Allocator/StackAllocator.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            class VulkanDevice;
            class VulkanCommandList;
            class VulkanCommandPool : public RHICommandPool {
            protected:
                Core::Allocator* m_allocator;
                const VulkanDevice& m_device;
                vk::raii::CommandPool m_commandPool{ nullptr };
                RHIObjectStorage<> m_storage;
                constexpr static size_t kCommandListReserveSize = 256;
            public:
                VulkanCommandPool(const VulkanDevice& device, PoolDesc const& desc, Core::Allocator* allocator);

                inline auto const& GetDevice() const { return m_device; }
                inline auto const& GetVkCommandPool() const { return m_commandPool; }

                RHICommandPoolScopedHandle<RHICommandList> CreateCommandList() override;
                RHICommandList* GetCommandList(Handle handle) const override;
                void DestroyCommandList(Handle handle) override;

                inline bool IsValid() const { return m_commandPool != nullptr; }
                inline const char* GetName() const override { return "VulkanCommandPool"; }
            };
            class VulkanCommandList : public RHICommandList {
            protected:
                const VulkanCommandPool& m_commandPool;
                vk::raii::CommandBuffer m_commandBuffer{ nullptr };
                struct Barriers {
                    Core::StlVector<vk::ImageMemoryBarrier2> image;
                    Core::StlVector<vk::BufferMemoryBarrier2> buffer;
                    Barriers(Core::Allocator* allocator) : image(allocator), buffer(allocator) {};
                };
                Core::UniquePtr<Barriers> m_barriers;

                Core::ScopedArena m_arena;
                Core::StackAllocatorSingleThreaded m_allocator;
                constexpr static size_t kArenaSize = 2LL * (1LL << 20); // 2 MB
            public:
                VulkanCommandList(const VulkanCommandPool& commandPool);

                inline auto const& GetVkCommandBuffer() const { return m_commandBuffer; }

                inline bool IsValid() const { return m_commandBuffer != nullptr; }
                inline const char* GetName() const override { return "VulkanCommandList"; }

                RHICommandList& Begin() override;

                RHICommandList& BeginTransition() override;
                RHICommandList& SetBufferTransition(RHIBuffer* image, TransitionDesc const& desc) override;
                RHICommandList& SetImageTransition(RHIImage* image, TransitionDesc const& desc) override;
                RHICommandList& EndTransition() override;

                RHICommandList& SetPipeline(PipelineDesc const& desc) override;
                RHICommandList& SetViewport(float x, float y, float width, float height, float depth_min = 0.0, float depth_max = 1.0) override;
                RHICommandList& SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
                RHICommandList& Draw(uint32_t vertex_count, uint32_t instance_count = 1, uint32_t first_vertex = 0, uint32_t first_instance = 0) override;

                RHICommandList& BeginGraphics(GraphicsDesc const& desc) override;
                RHICommandList& BindVertexBuffer(uint32_t index, Core::StlSpan<RHIBuffer* const> buffers, Core::StlSpan<const size_t> offsets) override;
                RHICommandList& EndGraphics() override;

                void End() override;
                void Reset() override;
            };
        }
    }
}
