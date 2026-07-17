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
    auto surface_caps = mDevice.GetVkPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
    auto present_modes = mDevice.GetVkPhysicalDevice().getSurfacePresentModesKHR(surface);
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
    auto formats = mDevice.GetVkPhysicalDevice().getSurfaceFormatsKHR(surface);
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
    mSwapchain = vk::raii::SwapchainKHR(device, create_info, mDevice.GetVkAllocationCallbacks());
    auto images = mSwapchain.getImages();
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
uint32_t VulkanSwapchain::GetNextImage(uint64_t timeout_ns, RHIDeviceHandle<RHIDeviceSemaphore> semaphore, RHIDeviceHandle<RHIDeviceFence> fence)
{
    vk::Result result{};
    uint32_t index{};
    try
    {
        std::tie(result, index) = mSwapchain.acquireNextImage(
            timeout_ns,
            semaphore ? semaphore.Get<VulkanDeviceSemaphore>()->GetVkSemaphore() : vk::Semaphore(),
            fence ? fence.Get<VulkanDeviceFence>()->GetVkFence() : vk::Fence()
        );
    }
    catch (vk::SystemError&)
    {
        throw RHISwapchainResizeException();
    }
    switch (result)
    {
    case vk::Result::eErrorSurfaceLostKHR:
    case vk::Result::eErrorOutOfDateKHR:
    case vk::Result::eSuboptimalKHR:
        // Swapchain resize        
        throw RHISwapchainResizeException();
    default:
        // TODO: Handle other errors?
        break;
    }
    CHECK_MSG(index < mImagesPtrs.size(), "Swapchain image index {} out of range ({}). Result={}", index, mImagesPtrs.size(), static_cast<int>(result))
    return index;
}

void VulkanSwapchain::DebugSetObjectName(const char* name) {
    VkSwapchainKHR handle = *mSwapchain;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eSwapchainKHR,
        .objectHandle = reinterpret_cast<uint64_t>(handle),
        .pObjectName = name
        });
}
