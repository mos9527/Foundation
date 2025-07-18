#pragma once
#include <Platform/RHI/Common.hpp>
#include <Platform/RHI/Resource.hpp>
#include <Platform/RHI/Swapchain.hpp>
#include <Platform/RHI/PipelineState.hpp>
#include <Platform/RHI/Descriptor.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            class RHIDevice;
            class RHIDeviceQueue;
            class RHICommandList;
            class RHICommandPool;
            template<typename T> using RHICommandPoolHandle = RHIHandle<RHICommandPool, T>;
            template<typename T> using RHICommandPoolScopedHandle = RHIScopedHandle<RHICommandPool, T>;
            class RHICommandPool : public RHIObject {
            protected:
                const RHIDevice& m_device;
            public:
                struct PoolDesc {
                    // Queue this command pool is associated with.
                    RHIDeviceQueueType queue;
                    // How CommandList should be created by this pool.
                    RHICommandPoolType type;
                } const m_desc;               
                RHICommandPool(RHIDevice const& device, PoolDesc desc) : m_device(device), m_desc(desc) {}

                virtual RHICommandPoolScopedHandle<RHICommandList> CreateCommandList() = 0;
                virtual RHICommandList* GetCommandList(Handle handle) const = 0;
                virtual void DestroyCommandList(Handle handle) = 0;
            };            
            class RHICommandList : public RHIObject {
            protected:
                const RHICommandPool& m_commandPool;
             
            public:
                RHICommandList(RHICommandPool const& commandPool) : m_commandPool(commandPool) {}
#pragma region Transition
                struct TransitionDesc {
                    RHIResourceAccess src_access, dst_access;
                    RHIPipelineStage src_stage, dst_stage;
                    // Image
                    RHIImageLayout src_img_layout, dst_img_layout;
                    RHIImageSubresourceRange src_img_range{};
                    // Buffer
                    size_t src_buffer_offset = 0, src_buffer_size = kFullSize;
                };
                virtual RHICommandList& BeginTransition() = 0;
                virtual RHICommandList& SetBufferTransition(RHIBuffer* buffer, TransitionDesc const& desc) = 0;
                virtual RHICommandList& SetImageTransition(RHIImage* image, TransitionDesc const& desc) = 0;
                virtual RHICommandList& EndTransition() = 0;
#pragma endregion                
#pragma region PSO
                struct PipelineDesc {
                    RHIPipelineState* pipeline;
                    RHIDevicePipelineType type;
                };
                virtual RHICommandList& SetPipeline(PipelineDesc const& desc) = 0;           
                virtual RHICommandList& BindDescriptorSet(
                    RHIDevicePipelineType bindpoint,
                    RHIPipelineState* pipeline,
                    Core::StlSpan<RHIDeviceDescriptorSet* const> sets,
                    size_t first = 0) = 0;
#pragma endregion
#pragma region Rasterizer
                virtual RHICommandList& SetViewport(float x, float y, float width, float height, float depth_min = 0.0, float depth_max = 1.0) = 0;
                virtual RHICommandList& SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;
                virtual RHICommandList& Draw(uint32_t vertex_count, uint32_t instance_count = 1, uint32_t first_vertex = 0, uint32_t first_instance = 0) = 0;
                virtual RHICommandList& DrawIndexed(uint32_t index_count, uint32_t instance_count = 1, uint32_t first_index = 0, int32_t vertex_offset = 0, uint32_t first_instance = 0) = 0;
#pragma endregion
#pragma region Transfer Queue
                struct CopyBufferRegion {
                    size_t src_offset = 0;
                    size_t dst_offset = 0;
                    /// Size of the region to copy.
                    /// If size is kFullSize, the maximum copiable region
                    /// min(src_buffer.size - src_offset, dst_buffer.size - dst_offset)
                    /// will be used.
                    size_t size = kFullSize;
                };
                virtual RHICommandList& CopyBuffer(RHIBuffer* src_buffer, RHIBuffer* dst_buffer, Core::StlSpan<const CopyBufferRegion> regions) = 0;
                struct CopyImageRegion {
                    uint32_t src_buffer_offset = 0; // Offset in the source buffer, used for CopyBufferToImage
                    RHIImageSubresourceLayer src_layer;
                    RHIOffset3D src_offset{ 0,0,0 };
                    RHIImageSubresourceLayer dst_layer;
                    RHIOffset3D dst_offset{ 0,0,0 };
                    /// Extent of the region to copy.
                    /// This MUST have a non-zero size (size=xyz)
                    /// or the call to Copy(...)Image is invalid.
                    RHIExtent3D extent{ 0,0,0 };
                };
                virtual RHICommandList& CopyImage(RHIImage* src_image, RHIImageLayout src_layout, RHIImage* dst_image, RHIImageLayout dst_layout, Core::StlSpan<const CopyImageRegion> regions) = 0;
                virtual RHICommandList& CopyBufferToImage(RHIBuffer* src_buffer, RHIImage* dst_image, RHIImageLayout dst_layout, Core::StlSpan<const CopyImageRegion> regions) = 0;
#pragma endregion
#pragma region Graphics Pipeline
                struct GraphicsDesc {
                    struct Attachment {
                        RHIImageView* image_view;
                        RHIImageLayout image_layout;
                        RHIClearColor clear_color;
                    };
                    Core::StlSpan<const Attachment> attachments;
                    uint32_t width, height;
                };
                virtual RHICommandList& BeginGraphics(GraphicsDesc const& desc) = 0;
                virtual RHICommandList& BindVertexBuffer(uint32_t index, Core::StlSpan<RHIBuffer* const> buffers, Core::StlSpan<const size_t> offsets) = 0;
                virtual RHICommandList& BindIndexBuffer(RHIBuffer* buffer, size_t offset = 0, RHIResourceFormat format = RHIResourceFormat::R32_UINT) = 0;
                virtual RHICommandList& EndGraphics() = 0;
#pragma endregion
#pragma region Tags
                virtual RHICommandList& Begin() = 0;
                virtual void End() = 0;
                virtual void Reset() = 0;
#pragma endregion
            };

            template<> struct RHIObjectTraits<RHICommandList> {
                static RHICommandList* Get(RHICommandPool const* pool, Handle handle) {
                    return pool->GetCommandList(handle);
                }
                static void Destroy(RHICommandPool* pool, Handle handle) {
                    pool->DestroyCommandList(handle);
                }
            };
        }
    }
}
