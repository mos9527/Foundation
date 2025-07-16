#pragma once
#include <Platform/RHI/Swapchain.hpp>
#include <Platform/RHI/Vulkan/Common.hpp>
#include <Platform/RHI/Vulkan/Resource.hpp>
#include <Core/Allocator/StlContainers.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            inline vk::PresentModeKHR GetVulkanPresentModeFromSwapchainDesc(RHISwapchain::SwapchainDesc::PresentMode mode) {
                using enum RHISwapchain::SwapchainDesc::PresentMode;
                switch (mode) {
                case MAILBOX: return vk::PresentModeKHR::eMailbox;
                case IMMEDIATE: return vk::PresentModeKHR::eImmediate;
                case FIFO:
                default:
                    return vk::PresentModeKHR::eFifo;
                }
            }
            class VulkanDevice;
            class VulkanSwapchain : public RHISwapchain {
                const VulkanDevice& m_device;
                vk::raii::SwapchainKHR m_swapchain{ nullptr };
                std::array<uint32_t, 2> m_queue_family_indices;
                vk::SwapchainCreateInfoKHR GetSwapchainCreateInfo();

                RHIObjectStorage<VulkanImage> m_images;
                Core::StlVector<RHIImage*> m_images_ptrs;
                void Instantiate();
            public:
                VulkanSwapchain(const VulkanDevice& device, SwapchainDesc const& desc);

                Core::StlSpan<RHIImage* const> GetImages() const override;
                inline bool IsValid() const override { return m_swapchain != nullptr; }
                inline const char* GetName() const override { return m_desc.name.c_str(); }

                inline auto const& GetVkSwapchain() const { return m_swapchain; }

                size_t GetNextImage(
                    uint64_t timeout_ns,
                    RHIDeviceObjectHandle<RHIDeviceSemaphore> semaphore,
                    RHIDeviceObjectHandle<RHIDeviceFence> fence
                ) override;
            };
        }
    }
}
