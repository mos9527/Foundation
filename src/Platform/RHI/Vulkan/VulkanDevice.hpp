#pragma once
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vma/vk_mem_alloc.h>

#include <Platform/RHI/Vulkan/VulkanApplication.hpp>
namespace Foundation {
    namespace Platform {
        constexpr uint32_t kInvalidQueueIndex = -1;
        class VulkanSwapchain;
        class VulkanDevice : public RHIDevice {
            const VulkanApplication& m_app;
            /// Actual device instance
            vk::raii::PhysicalDevice m_physicalDevice{ nullptr };
            vk::raii::Device m_device{ nullptr };
            vk::PhysicalDeviceProperties m_properties;
            /// Surface for Present
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
                        transfer != kInvalidQueueIndex &&
                        present != kInvalidQueueIndex;
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
            VmaAllocator m_allocator{ nullptr };
        public:
            VulkanDevice(VulkanApplication const& app, const vk::raii::PhysicalDevice& physicalDevice)
                : RHIDevice(app), m_app(app), m_physicalDevice(physicalDevice) {
                m_properties = m_physicalDevice.getProperties();
            }
            ~VulkanDevice();

            void DebugLogDeviceInfo() const;
            void DebugLogAllocatorInfo() const;

            void Instantiate(Window*) override;
            inline const char* GetName() const override { return m_properties.deviceName.data(); }
            inline bool IsValid() const override { return m_device != nullptr; }

            inline auto const& GetQueues() const { return m_queues; }
            inline auto const& GetDevice() const { return m_device; }
            inline auto const& GetSurface() const { return m_surface; }
            inline auto const& GetAllocator() const { return m_allocator; }
            inline auto const& GetPhysicalDevice() const { return m_physicalDevice; }

            std::unique_ptr<RHISwapchain> CreateSwapchain(RHISwapchain::SwapchainDesc const& desc) const override;
        };
    }
}
