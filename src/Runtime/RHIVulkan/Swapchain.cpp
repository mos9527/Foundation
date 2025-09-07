#include "Resource.hpp"
#include "Device.hpp"
#include "Swapchain.hpp"
using namespace Foundation;
using namespace Foundation::RHI;
const vk::SwapchainCreateInfoKHR VulkanSwapchain::vkSwapchainCreateInfoFromSwapchainDesc(SwapchainDesc const& desc) {
    auto const& surface = m_device.GetVkSurface();
    auto surface_caps = m_device.GetVkPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
    auto present_modes = m_device.GetVkPhysicalDevice().getSurfacePresentModesKHR(surface);
    // Validate requested parameters
    CHECK_MSG(
        desc.extents.x >= surface_caps.minImageExtent.width && desc.extents.x <= surface_caps.maxImageExtent.width,
        "Swapchain extent width {} not supported (min {}, max {})",
        desc.extents.x, surface_caps.minImageExtent.width, surface_caps.maxImageExtent.width
    );
    CHECK_MSG(
        desc.extents.y >= surface_caps.minImageExtent.height && desc.extents.y <= surface_caps.maxImageExtent.height,
        "Swapchain extent height {} not supported (min {}, max {})",
        desc.extents.y, surface_caps.minImageExtent.height, surface_caps.maxImageExtent.height
    );
    CHECK_MSG(
        std::find(present_modes.begin(), present_modes.end(), vkPresentModeFromSwapchainDesc(desc.present_mode)) != present_modes.end(),
        "Swapchain present mode {} not supported",
        (uint32_t)desc.present_mode
    );
    CHECK_MSG(
        desc.min_buffer_count >= surface_caps.minImageCount,
        "Swapchain min buffer count {} not supported (min {})",
        desc.min_buffer_count, surface_caps.minImageCount
    );
    vk::Format vk_format = vkFormatFromRHIFormat(desc.format);
    std::optional<vk::ColorSpaceKHR> color_space;
    auto formats = m_device.GetVkPhysicalDevice().getSurfaceFormatsKHR(surface);
    for (auto const& fmt : formats) {
        if (fmt.format == vk_format) {
            color_space = fmt.colorSpace;
            break;
        }
    }
    CHECK_MSG(color_space.has_value(), "Swapchain format {} not supported", desc.format);
    vk::SwapchainCreateInfoKHR create_info{
        .surface = surface,
        .minImageCount = desc.min_buffer_count,
        .imageFormat = vk_format,
        .imageColorSpace = color_space.value(),
        .imageExtent = vk::Extent2D(desc.extents.x, desc.extents.y),
        .imageArrayLayers = 1, // 1 layer for 2D images
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
        .imageSharingMode = vk::SharingMode::eExclusive, // Exclusive mode by default
        .preTransform = surface_caps.currentTransform,
        .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque, // Opaque composite alpha
        .presentMode = vkPresentModeFromSwapchainDesc(desc.present_mode),
        .clipped = VK_TRUE, // Clipped presentation
        .oldSwapchain = m_swapchain // No old swapchain
    };
    // Check for different graphics-present queue indices
    auto const& queues = m_device.GetVkQueues();
    m_queue_family_indices = {
        queues->Get(queues->graphics)->GetVkQueueIndex(),
        queues->Get(queues->present)->GetVkQueueIndex()
    };
    if (queues->IsDedicatedPresent()) {
        create_info.setImageSharingMode(vk::SharingMode::eConcurrent);
        create_info.setQueueFamilyIndices(m_queue_family_indices);
    }
    return create_info;
}
void VulkanSwapchain::Instantiate() {
    CHECK_MSG(m_device.GetVkQueues() && m_device.GetVkQueues()->CanPresent(), "Device does not have a present-capable queue");
    auto const& device = m_device.GetVkDevice();
    auto create_info = vkSwapchainCreateInfoFromSwapchainDesc(m_desc);
    m_images.Clear(), m_images_ptrs.clear();
    m_swapchain = vk::raii::SwapchainKHR(device, create_info, m_device.GetVkAllocatorCallbacks());
    auto images = m_swapchain.getImages();
    for (auto& image : images) {
        const Handle handle = m_images.CreateObject<VulkanTexture>(m_device, RHITextureDesc{}, vk::raii::Image(device, image, m_device.GetVkAllocatorCallbacks()), true /*shared=true*/);
        m_images_ptrs.push_back(m_images.GetObjectPtr(handle));
    }
}
VulkanSwapchain::VulkanSwapchain(const VulkanDevice& device, SwapchainDesc const& desc)
    : RHISwapchain(device, desc), m_device(device), m_images(device.GetAllocator()), m_images_ptrs(device.GetAllocator()) {
    Instantiate();
}
Core::StlSpan<RHITexture* const> VulkanSwapchain::GetImages() const {
    return m_images_ptrs;
}
RHIExtent2D VulkanSwapchain::GetExtents() const
{
    return m_desc.extents;
}
uint32_t VulkanSwapchain::GetNextImage(uint64_t timeout_ns, RHIDeviceObjectHandle<RHIDeviceSemaphore> semaphore, RHIDeviceObjectHandle<RHIDeviceFence> fence)
{
    auto [result, index] = m_swapchain.acquireNextImage(
        timeout_ns,
        semaphore ? semaphore.Get<VulkanDeviceSemaphore>()->GetVkSemaphore() : vk::Semaphore(),
        fence ? fence.Get<VulkanDeviceFence>()->GetVkFence() : vk::Fence()
    );
    switch (result)
    {
    case vk::Result::eErrorOutOfDateKHR:
    case vk::Result::eSuboptimalKHR:
        // Swapchain resize        
        throw RHISwapchainResizeException();
    default:
        // TODO: Handle other errors?
        break;
    }
    return index;
}

void VulkanSwapchain::DebugSetObjectName(const char* name) {
    VkSwapchainKHR handle = *m_swapchain;
    m_device.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eSwapchainKHR,
        .objectHandle = reinterpret_cast<uint64_t>(handle),
        .pObjectName = name
        });
}
