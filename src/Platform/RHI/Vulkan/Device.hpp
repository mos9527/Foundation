#pragma once
#include <Platform/RHI/Device.hpp>
#include <Platform/RHI/Vulkan/Common.hpp>
#include <vma/vk_mem_alloc.h>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            constexpr uint32_t kInvalidQueueIndex = -1;
            class VulkanApplication;
            class VulkanDevice : public RHIDevice {
                const VulkanApplication& m_app;
                vk::PhysicalDeviceProperties m_properties;
                vk::raii::PhysicalDevice m_physicalDevice{ nullptr };
                vk::raii::Device m_device{ nullptr };
                vk::raii::SurfaceKHR m_surface{ nullptr };
                struct VulkanDeviceQueues
                {
                    uint32_t graphics = kInvalidQueueIndex;
                    uint32_t compute = kInvalidQueueIndex;
                    uint32_t transfer = kInvalidQueueIndex;
                    uint32_t present = kInvalidQueueIndex;
                    inline bool IsValid() const {
                        return graphics != kInvalidQueueIndex &&
                            compute != kInvalidQueueIndex &&
                            transfer != kInvalidQueueIndex;
                    }
                    inline bool CanPresent() const {
                        return present != kInvalidQueueIndex;
                    }
                    inline bool IsDedicatedCompute() const {
                        return IsValid() && compute != graphics;
                    }
                    inline bool IsDedicatedTransfer() const {
                        return IsValid() && transfer != graphics;
                    }
                    inline bool IsDedicatedPresent() const {
                        return IsValid() && present != graphics;
                    }
                } m_queues;
                VmaAllocator m_vkAllocator{ nullptr };
                Core::Allocator* m_allocator;
                // Device Object storage
                // Lifetimes and handle dereferencing are managed by the device.

                RHIObjectStorage m_storage;
                void Instantiate(Window* win = nullptr);
            public:
                VulkanDevice(Window* window, VulkanApplication const& app, const vk::raii::PhysicalDevice& physicalDevice, Core::Allocator* allocator);
                ~VulkanDevice();

                void DebugLogDeviceInfo() const;
                void DebugLogAllocatorInfo() const;

                RHIDeviceScopedObjectHandle<RHISwapchain> CreateSwapchain(RHISwapchain::SwapchainDesc const& desc) override;
                RHISwapchain* GetSwapchain(Handle handle) const override;
                void DestroySwapchain(Handle handle) override;

                RHIDeviceScopedObjectHandle<RHIPipelineState> CreatePipelineState(RHIPipelineState::PipelineStateDesc const& desc) override;
                RHIPipelineState* GetPipelineState(Handle handle) const override;
                void DestroyPipelineState(Handle handle) override;

                RHIDeviceScopedObjectHandle<RHIShaderModule> CreateShaderModule(RHIShaderModule::ShaderModuleDesc const& desc) override;
                RHIShaderModule* GetShaderModule(Handle handle) const override;
                void DestroyShaderModule(Handle handle) override;

                RHIDeviceScopedObjectHandle<RHIShaderPipelineModule> CreateShaderPipelineModule
                (RHIShaderPipelineModule::ShaderPipelineModuleDesc const& desc, RHIDeviceObjectHandle<RHIShaderModule> shader_module) override;
                RHIShaderPipelineModule* GetShaderPipelineModule(Handle handle) const override;
                void DestroyShaderPipelineModule(Handle handle) override;

                inline const char* GetName() const override { return m_properties.deviceName.data(); }
                inline bool IsValid() const override { return m_device != nullptr; }

                inline auto const& GetVkQueues() const { return m_queues; }
                inline auto const& GetVkDevice() const { return m_device; }
                inline auto const& GetVkSurface() const { return m_surface; }
                inline auto const& GetVkPhysicalDevice() const { return m_physicalDevice; }
            };
        }
    }
}
