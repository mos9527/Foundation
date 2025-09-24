#pragma once
#include <RHICore/Swapchain.hpp>
#include "Common.hpp"
#include "Resource.hpp"
namespace Foundation::RHI {
    inline vk::PresentModeKHR vkPresentModeFromSwapchainDesc(RHISwapchainPresentMode mode) {
        using enum RHISwapchainPresentMode;
        switch (mode) {
        case Mailbox: return vk::PresentModeKHR::eMailbox;
        case Tearing: return vk::PresentModeKHR::eImmediate;
        case Fifo:
        default:
            return vk::PresentModeKHR::eFifo;
        }
    }
    class VulkanDevice;
    class VulkanSwapchain : public RHISwapchain {
        const VulkanDevice& m_device;
        vk::raii::SwapchainKHR m_swapchain{ nullptr };
        std::array<uint32_t, 2> m_queue_family_indices{};
        UniquePtr<RHIObjectPool<VulkanTexture>> m_images;
        Core::Vector<RHITexture*> m_images_ptrs;

        void Instantiate();
        vk::SwapchainCreateInfoKHR vkSwapchainCreateInfoFromSwapchainDesc(SwapchainDesc const& desc);
    public:
        VulkanSwapchain(const VulkanDevice& device, SwapchainDesc const& desc);

        Core::Span<RHITexture* const> GetImages() const override;

        inline auto const& GetVkSwapchain() const { return m_swapchain; }
        RHIExtent2D GetExtents() const override;
        uint32_t GetNextImage(
            uint64_t timeout_ns,
            RHIDeviceObjectHandle<RHIDeviceSemaphore> semaphore,
            RHIDeviceObjectHandle<RHIDeviceFence> fence
        ) override;

        void DebugSetObjectName(const char* name) override;
    };
}
