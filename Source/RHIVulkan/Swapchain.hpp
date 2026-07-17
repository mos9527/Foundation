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
        const VulkanDevice& mDevice;
        vk::raii::SwapchainKHR mSwapchain{ nullptr };
        std::array<uint32_t, 2> mQueueFamilyIndices{};
        UniquePtr<RHIObjectPool<VulkanTexture>> mImages;
        Vector<RHITexture*> mImagesPtrs;
        Vector<RHITextureScopedHandle<RHITextureView>> mViews;
        Vector<RHITextureView*> mViewsPtrs;

        void Instantiate();
        vk::SwapchainCreateInfoKHR vkSwapchainCreateInfoFromSwapchainDesc(SwapchainDesc desc);
    public:
        VulkanSwapchain(const VulkanDevice& device, SwapchainDesc const& desc);

        [[nodiscard]] Span<RHITexture* const> GetImages() const override;
        [[nodiscard]] Span<RHITextureView* const> GetViews() const override;

        [[nodiscard]] auto const& GetVkSwapchain() const { return mSwapchain; }
        [[nodiscard]] RHIExtent2D GetExtents() const override;
        uint32_t GetNextImage(
            uint64_t timeout_ns,
            RHIDeviceHandle<RHIDeviceSemaphore> semaphore,
            RHIDeviceHandle<RHIDeviceFence> fence
        ) override;

        void DebugSetObjectName(const char* name) override;
    };
}
