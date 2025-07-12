#include <Platform/RHI/Vulkan/Details/Resource.hpp>
#include <Platform/RHI/Vulkan/Device.hpp>
#include <Platform/RHI/Vulkan/Swapchain.hpp>
using namespace Foundation::Platform::RHI;
vk::SwapchainCreateInfoKHR VulkanSwapchain::GetSwapchainCreateInfo() {
    auto const& surface = m_device.GetVkSurface();
    auto surface_caps = m_device.GetVkPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
    vk::SwapchainCreateInfoKHR create_info{
        .surface = surface,
        .minImageCount = m_desc.buffer_count,
        .imageFormat = GetVulkanFormatFromRHIFormat(m_desc.format),
        .imageColorSpace = vk::ColorSpaceKHR::eSrgbNonlinear,
        .imageExtent = vk::Extent2D(m_desc.width, m_desc.height),
        .imageArrayLayers = 1, // 1 layer for 2D images
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
        .imageSharingMode = vk::SharingMode::eExclusive, // Exclusive mode by default
        .preTransform = surface_caps.currentTransform,
        .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque, // Opaque composite alpha
        .presentMode = GetVulkanPresentModeFromSwapchainDesc(m_desc.present_mode),
        .clipped = VK_TRUE, // Clipped presentation
        .oldSwapchain = m_swapchain // No old swapchain
    };
    // Check for different graphics-present queue indices
    auto const& queues = m_device.GetVkQueues();
    m_queue_family_indices = { queues.graphics, queues.present };
    if (queues.IsDedicatedPresent()) {
        create_info.setImageSharingMode(vk::SharingMode::eConcurrent);
        create_info.setQueueFamilyIndices(m_queue_family_indices);
    }
    return create_info;
}
void VulkanSwapchain::Instantiate() {
    if (!m_device.GetVkQueues().CanPresent())
        throw std::runtime_error("device does not support presentation");
    auto const& device = m_device.GetVkDevice();
    auto create_info = GetSwapchainCreateInfo();
    m_swapchain = vk::raii::SwapchainKHR(device, create_info);
    m_images = m_swapchain.getImages();
    // Create ImageViews for each buffer
    m_image_views.clear();
    m_image_views.reserve(m_images.size());
    for (const auto& image : m_images) {
        vk::ImageViewCreateInfo view_info{
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = GetVulkanFormatFromRHIFormat(m_desc.format),
            .components = {},
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 }
        };
        m_image_views.emplace_back(device, view_info);
    }
}
VulkanSwapchain::VulkanSwapchain(const VulkanDevice& device, SwapchainDesc const& desc)
    : RHISwapchain(device, desc), m_device(device) {
    Instantiate();
}
