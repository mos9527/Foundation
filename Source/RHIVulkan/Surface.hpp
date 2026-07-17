#pragma once
#include <RHICore/Surface.hpp>
#include "Common.hpp"

namespace Foundation::RHI {
    class VulkanDevice;

    class VulkanSurface : public RHISurface {
        const VulkanDevice& mDevice;
        vk::raii::SurfaceKHR mSurface{ nullptr };
        Vector<RHISurfaceFormat> mSupportedFormats;
        Vector<RHISwapchainPresentMode> mSupportedPresentModes;

    public:
        VulkanSurface(const VulkanDevice& device, SurfaceDesc const& desc);
        
        [[nodiscard]] auto const& GetVkSurface() const { return mSurface; }

        [[nodiscard]] Span<RHISurfaceFormat const> GetSupportedFormats() const override { return mSupportedFormats; }
        [[nodiscard]] Span<RHISwapchainPresentMode const> GetSupportedPresentModes() const override { return mSupportedPresentModes; }

        void DebugSetObjectName(const char* name) override {}
    };
}
