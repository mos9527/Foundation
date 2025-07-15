#pragma once
#include <Core/Allocator/Allocator.hpp>
#include <Platform/RHI/Application.hpp>
#include <Platform/RHI/Resource.hpp>
#include <Platform/RHI/Device.hpp>
namespace Foundation {
    namespace Renderer {
        using namespace Platform::RHI;
        class Renderer {
            Core::Allocator* m_allocator{ nullptr };

            RHIApplicationObjectHandle<RHIDevice> m_device;

            RHIDeviceScopedObjectHandle<RHIShaderModule> m_shader_vert, m_shader_frag;
            RHIDeviceScopedObjectHandle<RHISwapchain> m_swapchain;

            RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> m_sync_present, m_sync_draw;
            RHIDeviceScopedObjectHandle<RHIDeviceFence> m_fence_draw;

            RHIDeviceQueue* m_queue{ nullptr };

            RHIDeviceScopedObjectHandle<RHICommandPool> m_cmd_pool;
            RHICommandPoolScopedHandle<RHICommandList> m_cmd;

            RHIDeviceScopedObjectHandle<RHIPipelineState> m_pso;

            std::vector<RHIImageScopedHandle<RHIImageView>, Core::StlAllocator<RHIImageScopedHandle<RHIImageView>>>
                m_swapchain_imageviews;
            void Record(uint32_t image_index, RHICommandList* cmd);
        public:
            Renderer(RHIApplicationObjectHandle<RHIDevice> device, Core::Allocator* allocator);
            void Draw();
            ~Renderer();
        };
    }
}
