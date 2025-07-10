#include <Platform/RHI/Vulkan/VulkanDevice.hpp>
#include <Platform/RHI/Vulkan/VulkanSwapchain.hpp>

using namespace Foundation::Platform;
vk::SwapchainCreateInfoKHR VulkanSwapchain::GetSwapchainCreateInfo() {
    auto const& surface = m_device.GetSurface();
    auto surface_caps = m_device.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
    vk::SwapchainCreateInfoKHR create_info(
        {},
        m_device.GetSurface(),
        m_desc.buffer_count,
        GetVulkanFormatFromRHIFormat(m_desc.format),
        vk::ColorSpaceKHR::eSrgbNonlinear,
        vk::Extent2D(m_desc.width, m_desc.height),
        1,
        vk::ImageUsageFlagBits::eColorAttachment,
        vk::SharingMode::eExclusive,
        {},
        surface_caps.currentTransform,
        vk::CompositeAlphaFlagBitsKHR::eOpaque,
        GetVulkanPresentModeFromSwapchainDesc(m_desc.present_mode),
        VK_TRUE, m_swapchain, nullptr
    );
    // Check for different graphics-present queue indices
    auto const& queues = m_device.GetQueues();
    m_queue_family_indices = { queues.graphics, queues.present };
    if (queues.IsDedicatedPresent()) {
        create_info.setImageSharingMode(vk::SharingMode::eConcurrent);
        create_info.setQueueFamilyIndices(m_queue_family_indices);
    }
    return create_info;
}
void VulkanSwapchain::Instantiate() {
    auto const& device = m_device.GetDevice();
    auto create_info = GetSwapchainCreateInfo();
    m_swapchain = vk::raii::SwapchainKHR(device, create_info);
    m_images = m_swapchain.getImages();
    // Create ImageViews for each buffer
    m_image_views.clear();
    m_image_views.reserve(m_images.size());
    for (const auto& image : m_images) {
        vk::ImageViewCreateInfo view_info(
            {},
            image,
            vk::ImageViewType::e2D,
            GetVulkanFormatFromRHIFormat(m_desc.format),
            {}, // No swizzle
            { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 } // 1 layer, 1 mip level
        );
        m_image_views.emplace_back(device, view_info);
    }
}
VulkanSwapchain::VulkanSwapchain(const VulkanDevice& device, SwapchainDesc const& desc)
    : RHISwapchain(device, desc), m_device(device) {
    Instantiate();
}
