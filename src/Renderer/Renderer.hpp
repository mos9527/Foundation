#pragma once
#include <RHICore/Application.hpp>
#include <RHICore/Resource.hpp>
#include <RHICore/Device.hpp>
#include <RHICore/Descriptor.hpp>
namespace Foundation {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    class Renderer {
        Allocator* m_allocator{ nullptr };

        RHIApplicationObjectHandle<RHIDevice> m_device;
        RHIDeviceQueue* m_queue{ nullptr };

        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> m_desc_pool;
        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout> m_desc_layout;
        StlVector<RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet>> m_desc_set;

        RHIDeviceScopedObjectHandle<RHIShaderModule> m_shader_vert, m_shader_frag;
        RHIDeviceScopedObjectHandle<RHISwapchain> m_swapchain;

        RHIDeviceScopedObjectHandle<RHICommandPool> m_cmd_pool;
        struct PerSwapResources {
            RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> m_sync_present, m_sync_draw;
            RHIDeviceScopedObjectHandle<RHIDeviceFence> m_fence_draw;
            RHICommandPoolScopedHandle<RHICommandList> m_cmd;
            RHIDeviceScopedObjectHandle<RHIBuffer> m_uniform_buffer;
            RHITextureScopedHandle<RHITextureView> m_swapchain_imageview;

            RHIDeviceScopedObjectHandle<RHITexture> m_depth;
            RHITextureScopedHandle<RHITextureView> m_depth_view;
        };
        StlVector<PerSwapResources> m_swaps;
        uint32_t m_current_swap{ 0 };



        RHIDeviceScopedObjectHandle<RHIPipelineState> m_pso;
        uint32_t m_num_indices{ 0 };
        RHIDeviceScopedObjectHandle<RHIBuffer> m_vertex_buffer, m_index_buffer;
        RHIDeviceScopedObjectHandle<RHIDeviceSampler> m_sampler;
        RHIDeviceScopedObjectHandle<RHITexture> m_tex;
        RHITextureScopedHandle<RHITextureView> m_tex_view;

        void Record(uint32_t image_index, RHICommandList* cmd);
    public:
        Renderer(RHIApplicationObjectHandle<RHIDevice> device, Allocator* allocator);
        ~Renderer();
        void Draw();
    };
}
