#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include "Application.hpp"
#include "Device.hpp"
#include "Swapchain.hpp"
#include "Surface.hpp"

using namespace Foundation::Core;
using namespace Foundation::RHI;
vk::SwapchainCreateInfoKHR VulkanSwapchain::vkSwapchainCreateInfoFromSwapchainDesc(SwapchainDesc desc)
{
    auto const& surface = static_cast<VulkanSurface*>(desc.surface.Get())->GetVkSurface();
    auto surface_caps = VkExpect(mDevice.GetVkPhysicalDevice().getSurfaceCapabilitiesKHR(surface));
    auto const& physical_device = mDevice.GetVkPhysicalDevice();
    auto present_modes = VkEnumerate<vk::PresentModeKHR>(
        [&](uint32_t* count, vk::PresentModeKHR* data) {
            return physical_device.getDispatcher()->vkGetPhysicalDeviceSurfacePresentModesKHR(
                static_cast<VkPhysicalDevice>(*physical_device), static_cast<VkSurfaceKHR>(*surface),
                count, reinterpret_cast<VkPresentModeKHR*>(data));
        }, mDevice.GetAllocator());
    // Validate requested parameters
    desc.extents.x = std::clamp(desc.extents.x, surface_caps.minImageExtent.width, surface_caps.maxImageExtent.width);
    desc.extents.y = std::clamp(desc.extents.y, surface_caps.minImageExtent.height, surface_caps.maxImageExtent.height);
    if (desc.minBufferCount < surface_caps.minImageCount)
        desc.minBufferCount = surface_caps.minImageCount;
    if (surface_caps.maxImageCount > 0 && desc.minBufferCount > surface_caps.maxImageCount)
        desc.minBufferCount = surface_caps.maxImageCount;
    CHECK_MSG(
        Ranges::find(present_modes, vkPresentModeFromSwapchainDesc(desc.presentMode)) != present_modes.end(),
        "Swapchain present mode {} not supported",
        static_cast<uint32_t>(desc.presentMode));
    CHECK_MSG(
        desc.minBufferCount >= surface_caps.minImageCount,
        "Swapchain min buffer count {} not supported (min {})",
        desc.minBufferCount, surface_caps.minImageCount
    );
    vk::Format vk_format = vkFormatFromRHIFormat(desc.format);
    vk::ColorSpaceKHR vk_color_space = vkColorSpaceFromRHIColorSpace(desc.colorSpace);
    bool format_supported = false;
    auto formats = VkEnumerate<vk::SurfaceFormatKHR>(
        [&](uint32_t* count, vk::SurfaceFormatKHR* data) {
            return physical_device.getDispatcher()->vkGetPhysicalDeviceSurfaceFormatsKHR(
                static_cast<VkPhysicalDevice>(*physical_device), static_cast<VkSurfaceKHR>(*surface),
                count, reinterpret_cast<VkSurfaceFormatKHR*>(data));
        }, mDevice.GetAllocator());
    for (auto const& fmt : formats) {
        if (fmt.format == vk_format && fmt.colorSpace == vk_color_space) {
            format_supported = true;
            break;
        }
    }
    CHECK_MSG(format_supported, "Swapchain format {} with color space {} not supported", desc.format, desc.colorSpace);
    vk::SwapchainCreateInfoKHR create_info{
        .surface = surface,
        .minImageCount = desc.minBufferCount,
        .imageFormat = vk_format,
        .imageColorSpace = vk_color_space,
        .imageExtent = vk::Extent2D(desc.extents.x, desc.extents.y),
        .imageArrayLayers = 1, // 1 layer for 2D images
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eStorage,
        // ^^ AMD uses CS to clear themselves - not sure about NV drivers
        .imageSharingMode = vk::SharingMode::eExclusive, // Exclusive mode by default
        .preTransform = surface_caps.currentTransform,
        .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque, // Opaque composite alpha
        .presentMode = vkPresentModeFromSwapchainDesc(desc.presentMode),
        .clipped = VK_TRUE, // Clipped presentation
        .oldSwapchain = mSwapchain // No old swapchain
    };
    return create_info;
}
void VulkanSwapchain::Instantiate() {
    auto const& device = mDevice.GetVkDevice();
    auto create_info = vkSwapchainCreateInfoFromSwapchainDesc(mDesc);
    mImages.reset();
    mImages = ConstructUnique<RHIObjectPool<VulkanTexture>>(mDevice.GetAllocator(), mDevice.GetAllocator());
    mImagesPtrs.clear();
    mSwapchain = VkExpect(device.createSwapchainKHR(create_info, mDevice.GetVkAllocationCallbacks()));
    auto images = VkEnumerate<VkImage>(
        [&](uint32_t* count, VkImage* data) {
            return device.getDispatcher()->vkGetSwapchainImagesKHR(
                static_cast<VkDevice>(*device), static_cast<VkSwapchainKHR>(*mSwapchain), count, data);
        }, mDevice.GetAllocator());
    for (auto& image : images) {
        const Handle handle = mImages->CreateObject<VulkanTexture>(mDevice, RHITextureDesc{}, vk::raii::Image(device, image, mDevice.GetVkAllocationCallbacks()), true /*shared=true*/);
        mImagesPtrs.push_back(mImages->GetObjectPtr(handle));
    }
    // Update actual extents
    mDesc.extents.x = create_info.imageExtent.width;
    mDesc.extents.y = create_info.imageExtent.height;
    mDesc.minBufferCount = create_info.minImageCount;
}
VulkanSwapchain::VulkanSwapchain(const VulkanDevice& device, SwapchainDesc const& desc)
    : RHISwapchain(device, desc), mDevice(device), mImagesPtrs(device.GetAllocator()) {
    Instantiate();
}
Span<RHITexture* const> VulkanSwapchain::GetImages() const {
    return mImagesPtrs;
}
RHIExtent2D VulkanSwapchain::GetExtents() const
{
    return mDesc.extents;
}
RHISwapchainResult VulkanSwapchain::GetNextImage(uint64_t timeout_ns,
                                                RHIDeviceHandle<RHIDeviceSemaphore> semaphore,
                                                RHIDeviceHandle<RHIDeviceFence> fence, uint32_t& imageIndex)
{
    auto rv = mSwapchain.acquireNextImage(
        timeout_ns,
        semaphore ? semaphore.Get<VulkanDeviceSemaphore>()->GetVkSemaphore() : vk::Semaphore(),
        fence ? fence.Get<VulkanDeviceFence>()->GetVkFence() : vk::Fence()
    );
    vk::Result result = rv.result;
    imageIndex = rv.value;
    switch (result)
    {
    case vk::Result::eSuccess:
        CHECK_MSG(imageIndex < mImagesPtrs.size(), "Swapchain image index {} out of range ({})", imageIndex,
                  mImagesPtrs.size());
        return RHISwapchainResult::Success;
    case vk::Result::eSuboptimalKHR:
        CHECK_MSG(imageIndex < mImagesPtrs.size(), "Swapchain image index {} out of range ({})", imageIndex,
                  mImagesPtrs.size());
        return RHISwapchainResult::Suboptimal;
    case vk::Result::eNotReady:
        return RHISwapchainResult::NotReady;
    case vk::Result::eTimeout:
        return RHISwapchainResult::Timeout;
    case vk::Result::eErrorOutOfDateKHR:
        return RHISwapchainResult::OutOfDate;
    case vk::Result::eErrorSurfaceLostKHR:
        return RHISwapchainResult::SurfaceLost;
    case vk::Result::eErrorDeviceLost:
        return RHISwapchainResult::DeviceLost;
    default:
        return RHISwapchainResult::Error;
    }
}

void VulkanSwapchain::DebugSetObjectName(const char* name) {
    VkSwapchainKHR handle = *mSwapchain;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eSwapchainKHR,
        .objectHandle = reinterpret_cast<uint64_t>(handle),
        .pObjectName = name
        });
}
