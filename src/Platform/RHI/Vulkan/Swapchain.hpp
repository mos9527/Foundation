#pragma once
#include <Platform/RHI/Swapchain.hpp>
#include <Platform/RHI/Vulkan/Common.hpp>
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
                std::vector<vk::Image> m_images;
                std::vector<vk::raii::ImageView> m_image_views;

                std::vector<uint32_t> m_queue_family_indices;
                vk::SwapchainCreateInfoKHR GetSwapchainCreateInfo();

                void Instantiate();
            public:
                VulkanSwapchain(const VulkanDevice& device, SwapchainDesc const& desc);

                inline bool IsValid() const override { return m_swapchain != nullptr; }
                virtual inline const char* GetName() const override { return m_desc.name.c_str(); }
            };
        }
    }
}
