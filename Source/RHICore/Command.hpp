#pragma once
#include "Common.hpp"
#include "Resource.hpp"
#include "PipelineState.hpp"
#include "Descriptor.hpp"
namespace Foundation::RHI {
    class RHIDevice;
    class RHIDeviceQueue;
    class RHICommandList;
    class RHICommandPool;
    template<typename T> using RHICommandPoolHandle = RHIHandle<RHICommandPool, T>;
    template<typename T> using RHICommandPoolScopedHandle = RHIScopedHandle<RHICommandPool, T>;
    class RHICommandPool : public RHIObject {
    protected:
        const RHIDevice& mDevice;
    public:
        struct PoolDesc {
            // Queue this command pool is associated with.
            RHIDeviceQueue* queue;
            // How CommandList should be created by this pool.
            RHICommandPoolType type;
        } const mDesc;
        RHICommandPool(RHIDevice const& device, PoolDesc desc) : mDevice(device), mDesc(desc) {}

        [[nodiscard]] virtual RHICommandPoolScopedHandle<RHICommandList> CreateCommandList() = 0;
        [[nodiscard]] virtual RHICommandList* GetCommandList(Handle handle) const = 0;
        virtual void DestroyCommandList(Handle handle) = 0;
        
        virtual void ResetAllCommandLists(bool freeResources = false) = 0;

        virtual void DebugSetObjectName(const char* name) = 0;
    };
    const uint32_t kCommandQueueTransferIgnored = ~0u;
    class RHICommandList : public RHIObject {
    protected:
        const RHICommandPool& mCommandPool;
    public:
        RHICommandList(RHICommandPool const& commandPool) : mCommandPool(commandPool) {}
#pragma region Transition
        struct TransitionDesc {
            RHIResourceAccess srcAccess, dstAccess;
            RHIPipelineStage srcStage, dstStage;
            // Image
            RHITextureLayout srcImgLayout, dstImgLayout;
            RHITextureSubresourceRange srcImgRange{};
            // Buffer
            size_t srcBufferOffset = 0, srcBufferSize = kFullSize;
            // Queue Transfer
            uint32_t srcQueueIndex = kCommandQueueTransferIgnored, dstQueueIndex = kCommandQueueTransferIgnored;
        };
        virtual RHICommandList& BeginTransition() = 0;
        virtual RHICommandList& SetBufferTransition(RHIBuffer* buffer, TransitionDesc const& desc) = 0;
        virtual RHICommandList& SetImageTransition(RHITexture* image, TransitionDesc const& desc) = 0;
        virtual RHICommandList& EndTransition() = 0;
#pragma endregion                
#pragma region PSO
        struct PipelineDesc {
            RHIPipelineState* pipeline;
            RHIDevicePipelineType type;
        };
        virtual RHICommandList& SetPipeline(PipelineDesc const& desc) = 0;
        /**
         * @param dynamicOffsets Span of uint32_t mapping to *only* the dynamic bindings within the sets.
         *                       The indices are effectively the prefix sum of dynamic bindings in each set.
         */
        virtual RHICommandList& BindDescriptorSet(
            RHIDevicePipelineType bindpoint,
            RHIPipelineState* pipeline,
            Span<RHIDeviceDescriptorSet* const> sets,
            size_t first = 0,
            Span<uint32_t> dynamicOffsets = {}) = 0;
#pragma endregion
#pragma region Rasterizer
        virtual RHICommandList& SetViewport(float x, float y, float width, float height, float depth_min = 0.0, float depth_max = 1.0) = 0;
        virtual RHICommandList& SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;
        virtual RHICommandList& Draw(uint32_t vertex_count, uint32_t instance_count = 1, uint32_t first_vertex = 0, uint32_t first_instance = 0) = 0;
        virtual RHICommandList& DrawIndexed(uint32_t index_count, uint32_t instance_count = 1, uint32_t first_index = 0, int32_t vertex_offset = 0, uint32_t first_instance = 0) = 0;
        virtual RHICommandList& DrawIndexedIndirectCount(RHIBuffer* cmd_buffer, size_t cmd_offset, RHIBuffer* count_buffer, size_t count_offset, uint32_t max_draw_count, uint32_t cmd_stride) = 0;
        virtual RHICommandList& DrawMeshTasks(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) = 0;
        virtual RHICommandList& DrawMeshTasksIndirect(RHIBuffer* cmd_buffer, size_t cmd_offset, size_t draw_count, size_t stride) = 0;
#pragma endregion
        virtual RHICommandList& PushConstant(RHIPipelineState* pipeline, RHIShaderStage stage, uint32_t offset, Span<const char> data) = 0;
        virtual RHICommandList& UpdateBuffer(RHIBuffer* buffer, size_t offset, Span<const char> data) = 0;
#pragma region Transfer Queue
        struct CopyBufferRegion {
            size_t srcOffset = 0;
            size_t dstOffset = 0;
            /// Size of the region to copy.
            /// If size is kFullSize, the maximum copiable region
            /// min(src_buffer.size - src_offset, dst_buffer.size - dst_offset)
            /// will be used.
            size_t size = kFullSize;
        };
        virtual RHICommandList& FillBuffer(RHIBuffer* buffer, uint32_t value, size_t offset = 0, size_t size = kFullSize) = 0;
        virtual RHICommandList& CopyBuffer(RHIBuffer* src_buffer, RHIBuffer* dst_buffer, Span<const CopyBufferRegion> regions) = 0;
        struct CopyImageRegion {
            // Offset in the source buffer, used for CopyBufferToImage
            uint32_t srcBufferOffset = 0;
            RHITextureSubresourceLayer srcLayer;
            RHIOffset3D srcOffset{ 0,0,0 };
            RHITextureSubresourceLayer dstLayer;
            RHIOffset3D dstOffset{ 0,0,0 };
            /// Extent of the region to copy.
            /// This MUST have a non-zero size (size=xyz)
            /// or the call to Copy(...)Image is invalid.
            RHIExtent3D extent{ 0,0,0 };
        };
        virtual RHICommandList& CopyImage(RHITexture* src_image, RHITextureLayout src_layout, RHITexture* dst_image, RHITextureLayout dst_layout, Span<const CopyImageRegion> regions) = 0;
        virtual RHICommandList& CopyBufferToImage(RHIBuffer* src_buffer, RHITexture* dst_image, RHITextureLayout dst_layout, Span<const CopyImageRegion> regions) = 0;
#pragma endregion
#pragma region Graphics Pipeline
        struct GraphicsDesc {
            struct Attachment {
                RHITextureView* imageView{ nullptr };
                RHITextureLayout imageLayout{ RHITextureLayout::RenderTarget };
                // Clear values for the color attachment, if applicable.
                Optional<RHIClearColor> clearColor{};
                // Clear values for depth and stencil attachments, if applicable.
                // If both are set, the depth will be cleared first, then stencil.
                Optional<RHIClearDepthStencil> clearDepthStencil{};
                [[nodiscard]] constexpr bool IsValid() const
                {
                    return imageView;
                }
            };
            Span<const Attachment> colorAttachments;
            const Attachment depthAttachment{};
            const Attachment stencilAttachment{};
            uint32_t width, height;
        };
        virtual RHICommandList& BeginGraphics(GraphicsDesc const& desc) = 0;
        virtual RHICommandList& BindVertexBuffer(uint32_t index, Span<RHIBuffer* const> buffers, Span<const size_t> offsets) = 0;
        virtual RHICommandList& BindIndexBuffer(RHIBuffer* buffer, size_t offset = 0, RHIResourceFormat format = RHIResourceFormat::R32Uint) = 0;
        virtual RHICommandList& EndGraphics() = 0;
#pragma endregion
#pragma region Compute        
        virtual RHICommandList& Dispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) = 0;
#pragma endregion
#pragma region Tags
        virtual RHICommandList& DebugBegin(const char* message) = 0;
        virtual RHICommandList& DebugInsertMarker(const char* message) = 0;
        virtual RHICommandList& DebugEnd() = 0;
        virtual RHICommandList& Begin() = 0;
        virtual void End() = 0;
        virtual void Reset() = 0;
#pragma endregion

        virtual void DebugSetObjectName(const char* name) = 0;
    };

    template<> struct RHIObjectTraits<RHICommandPool, RHICommandList> {
        static RHICommandList* Get(RHICommandPool const* pool, Handle handle) {
            return pool->GetCommandList(handle);
        }
        static void Destroy(RHICommandPool* pool, Handle handle) {
            pool->DestroyCommandList(handle);
        }
    };
}
