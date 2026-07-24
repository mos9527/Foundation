#include "Surface.hpp"
#include "Device.hpp"
#include "Application.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace Foundation::RHI {
    VulkanSurface::VulkanSurface(const VulkanDevice& device, SurfaceDesc const& desc)
        : mDevice(device), mSupportedFormats(device.GetAllocator()), mSupportedPresentModes(device.GetAllocator()) {
        
        if (desc.windowHandle) {
            VkSurfaceKHR surface;
            CHECK_MSG(SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(desc.windowHandle), *device.GetApp().GetVkInstance(), device.GetApp().GetVkAllocationCallbacksNative(), &surface),
                      "failed to create window surface: {}", SDL_GetError());
            mSurface = vk::raii::SurfaceKHR(device.GetApp().GetVkInstance(), surface, device.GetApp().GetVkAllocationCallbacks());
            CHECK_MSG(VkExpect(device.GetVkPhysicalDevice().getSurfaceSupportKHR(device.GetGraphicsQueueFamilyIndex(), *mSurface), "getSurfaceSupportKHR"),
                      "Graphics queue family does not support the presentation surface");
        }

        if (*mSurface) {
            auto formats = VkExpect(device.GetVkPhysicalDevice().getSurfaceFormatsKHR(*mSurface), "getSurfaceFormatsKHR");
            for (auto& fmt : formats) {
                using enum RHIResourceFormat;
                using enum RHIColorSpace;
                RHIResourceFormat rhiFormat = Undefined;
                switch (fmt.format) {
                case vk::Format::eR8G8B8A8Unorm:
                    rhiFormat = R8G8B8A8Unorm; break;
                case vk::Format::eR8G8B8A8Srgb:
                    rhiFormat = R8G8B8A8Srgb; break;
                case vk::Format::eB8G8R8A8Unorm:
                    rhiFormat = B8G8R8A8Unrom; break;
                case vk::Format::eB8G8R8A8Srgb:
                    rhiFormat = B8G8R8A8Srgb; break;
                case vk::Format::eA2B10G10R10UnormPack32:
                    rhiFormat = A2B10G10R10Unorm; break;
                case vk::Format::eA2B10G10R10SnormPack32:
                    rhiFormat = A2B10G10R10Snorm; break;
                case vk::Format::eA2R10G10B10UnormPack32:
                    rhiFormat = A2R10G10B10Unorm; break;
                case vk::Format::eA2R10G10B10SnormPack32:
                    rhiFormat = A2R10G10B10Snorm; break;
                default: break;
                }
                
                if (rhiFormat != Undefined) {
                    RHIColorSpace rhiColorSpace = SrgbNonLinear;
                    switch (fmt.colorSpace) {
                    case vk::ColorSpaceKHR::eSrgbNonlinear:
                        rhiColorSpace = SrgbNonLinear; break;
                    case vk::ColorSpaceKHR::eHdr10St2084EXT:
                        rhiColorSpace = Hdr10St2084; break;
                    default: break;
                    }
                    mSupportedFormats.push_back({rhiFormat, rhiColorSpace});
                }
            }

            auto modes = VkExpect(device.GetVkPhysicalDevice().getSurfacePresentModesKHR(*mSurface), "getSurfacePresentModesKHR");
            for (auto& mode : modes) {
                switch (mode) {
                case vk::PresentModeKHR::eMailbox:
                    mSupportedPresentModes.push_back(RHISwapchainPresentMode::Mailbox); break;
                case vk::PresentModeKHR::eImmediate:
                    mSupportedPresentModes.push_back(RHISwapchainPresentMode::Tearing); break;
                case vk::PresentModeKHR::eFifo:
                    mSupportedPresentModes.push_back(RHISwapchainPresentMode::Fifo); break;
                default: break;
                }
            }
        }
    }
}
