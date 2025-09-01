#pragma once
#include <RHICore/Swapchain.hpp>
#include "Common.hpp"
#include "Resource.hpp"
namespace Foundation::RHI {
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
        RHIObjectStorage<VulkanTexture> m_images;
        Core::StlVector<RHITexture*> m_images_ptrs;

        void Instantiate();
        const vk::SwapchainCreateInfoKHR GetSwapchainCreateInfo(SwapchainDesc const& desc);
    public:
        VulkanSwapchain(const VulkanDevice& device, SwapchainDesc const& desc);

        Core::StlSpan<RHITexture* const> GetImages() const override;

        inline auto const& GetVkSwapchain() const { return m_swapchain; }
        RHIExtent2D GetDimensions() const override;
        uint32_t GetNextImage(
            uint64_t timeout_ns,
            RHIDeviceObjectHandle<RHIDeviceSemaphore> semaphore,
            RHIDeviceObjectHandle<RHIDeviceFence> fence
        ) override;
    };
}
