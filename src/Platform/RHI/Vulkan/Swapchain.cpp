#include <Platform/RHI/Vulkan/Resource.hpp>
#include <Platform/RHI/Vulkan/Device.hpp>
#include <Platform/RHI/Vulkan/Swapchain.hpp>
using namespace Foundation;
using namespace Foundation::Platform::RHI;
vk::SwapchainCreateInfoKHR VulkanSwapchain::GetSwapchainCreateInfo() {
    auto const& surface = m_device.GetVkSurface();
    auto surface_caps = m_device.GetVkPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
    vk::SwapchainCreateInfoKHR create_info{
        .surface = surface,
        .minImageCount = m_desc.buffer_count,
        .imageFormat = vkFormatFromRHIFormat(m_desc.format),
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
    if (!m_device.GetVkQueues() || !m_device.GetVkQueues()->CanPresent())
        throw std::runtime_error("invalid device or device does not support presentation");
    auto const& device = m_device.GetVkDevice();
    auto create_info = GetSwapchainCreateInfo();
    m_swapchain = vk::raii::SwapchainKHR(device, create_info, m_device.GetVkAllocatorCallbacks());
    auto images = m_swapchain.getImages();
    m_images.Clear(), m_images_ptrs.clear();
    for (auto& image : images) {
        Handle handle = m_images.CreateObject<VulkanImage>(m_device, vk::raii::Image(device, image, m_device.GetVkAllocatorCallbacks()), true /* shared */);
        m_images_ptrs.push_back(m_images.GetObjectPtr(handle));
    }
}
VulkanSwapchain::VulkanSwapchain(const VulkanDevice& device, SwapchainDesc const& desc)
    : RHISwapchain(device, desc), m_device(device), m_images(device.GetAllocator()), m_images_ptrs(device.GetAllocator()) {
    Instantiate();
}
Core::StlSpan<RHIImage* const> VulkanSwapchain::GetImages() const {
    return { m_images_ptrs.data(), m_images_ptrs.size() };
}

std::pair<size_t, size_t> VulkanSwapchain::GetDimensions() const
{
    // !! TODO: swapchain recreation
    return { m_desc.width, m_desc.height };
}

size_t VulkanSwapchain::GetNextImage(uint64_t timeout_ns, RHIDeviceObjectHandle<RHIDeviceSemaphore> semaphore, RHIDeviceObjectHandle<RHIDeviceFence> fence)
{
    auto [result, index] = m_swapchain.acquireNextImage(
        timeout_ns,
        semaphore ? semaphore.Get<VulkanDeviceSemaphore>()->GetVkSemaphore() : vk::Semaphore(),
        fence ? fence.Get<VulkanDeviceFence>()->GetVkFence() : vk::Fence()
    );
    // !! TODO handle errors
    return index;
}
