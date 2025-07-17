#pragma once
#include <Platform/RHI/Common.hpp>
#include <Platform/RHI/Resource.hpp>
#include <Platform/RHI/Swapchain.hpp>
#include <Platform/RHI/PipelineState.hpp>
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
                    RHIImageLayout src_img_layout, dst_img_layout;
                };
                virtual RHICommandList& BeginTransition() = 0;
                virtual RHICommandList& SetBufferTransition(RHIBuffer* buffer, TransitionDesc const& desc) = 0;
                virtual RHICommandList& SetImageTransition(RHIImage* image, TransitionDesc const& desc) = 0;
                virtual RHICommandList& EndTransition() = 0;
#pragma endregion                
#pragma region PSO
                struct PipelineDesc {
                    RHIPipelineState* pipeline;
                    enum class PipelineType {
                        Graphics,
                        Compute                        
                    } type;
                };
                virtual RHICommandList& SetPipeline(PipelineDesc const& desc) = 0;
#pragma endregion
#pragma region Rasterizer
                virtual RHICommandList& SetViewport(float x, float y, float width, float height, float depth_min = 0.0, float depth_max = 1.0) = 0;
                virtual RHICommandList& SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;
                virtual RHICommandList& Draw(uint32_t vertex_count, uint32_t instance_count = 1, uint32_t first_vertex = 0, uint32_t first_instance = 0) = 0;
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
