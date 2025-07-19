#pragma once
#include <Core/Allocator/StlContainers.hpp>
#include <Platform/RHI/Application.hpp>
#include <Platform/RHI/Resource.hpp>
#include <Platform/RHI/Device.hpp>
#include <Platform/RHI/Descriptor.hpp>
namespace Foundation {
    namespace Renderer {
        using namespace Platform::RHI;
        class Renderer {
            Core::Allocator* m_allocator{ nullptr };

            RHIApplicationObjectHandle<RHIDevice> m_device;
            RHIDeviceQueue* m_queue{ nullptr };

            RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> m_desc_pool;
            RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout> m_desc_layout;
            Core::StlVector<RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet>> m_desc_set;

            RHIDeviceScopedObjectHandle<RHIShaderModule> m_shader_vert, m_shader_frag;
            RHIDeviceScopedObjectHandle<RHISwapchain> m_swapchain;

            RHIDeviceScopedObjectHandle<RHICommandPool> m_cmd_pool;            
            Core::StlVector<RHIDeviceScopedObjectHandle<RHIDeviceSemaphore>> m_sync_present, m_sync_draw;
            Core::StlVector<RHIDeviceScopedObjectHandle<RHIDeviceFence>> m_fence_draw;
            Core::StlVector<RHICommandPoolScopedHandle<RHICommandList>> m_cmd;

            RHIDeviceScopedObjectHandle<RHIPipelineState> m_pso;
            RHIDeviceScopedObjectHandle<RHIBuffer> m_vertex_buffer, m_index_buffer;
            Core::StlVector<RHIDeviceScopedObjectHandle<RHIBuffer>> m_uniform_buffer;
            RHIDeviceScopedObjectHandle<RHIDeviceSampler> m_sampler;
            RHIDeviceScopedObjectHandle<RHIImage> m_tex;
            RHIImageScopedHandle<RHIImageView> m_tex_view;

            Core::StlVector<RHIImageScopedHandle<RHIImageView>>
                m_swapchain_imageviews;

            uint32_t m_current_img{ 0 };
            void Record(uint32_t image_index, RHICommandList* cmd);            
        public:
            Renderer(RHIApplicationObjectHandle<RHIDevice> device, Core::Allocator* allocator);
            ~Renderer();
            void Draw();
        };
    }
}
