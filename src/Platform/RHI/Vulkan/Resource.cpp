#include <Platform/RHI/Vulkan/Resource.hpp>
#include <Platform/RHI/Vulkan/Device.hpp>
using namespace Foundation::Platform::RHI;

VulkanResource::VulkanResource(VulkanDevice const& device, ResourceDesc const& desc) :
    RHIResource(device, desc), m_device(device) {
}

VulkanBuffer::VulkanBuffer(VulkanDevice const& device, BufferDesc const& desc)
    : RHIResource(device, desc), RHIBuffer(device, desc), VulkanResource(device, desc) {
    vk::BufferCreateInfo buffer_info{
        .size = m_desc.size,
        .usage = vkBufferUsageFromRHIBufferUsage(m_desc.usage),
        .sharingMode = m_desc.shared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive
    };
    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    auto allocator = device.GetVkAllocator();
    VkBuffer buffer;
    vmaCreateBuffer(allocator,
        reinterpret_cast<VkBufferCreateInfo*>(&buffer_info),
        &allocInfo,
        &buffer,
        &m_allocation,
        nullptr
    );
}
VulkanBuffer::~VulkanBuffer() {
    if (m_allocation) {
        auto allocator = m_device.GetVkAllocator();
        vmaDestroyBuffer(allocator, m_buffer.release(), m_allocation);
    }
}

VulkanImage::VulkanImage(VulkanDevice const& device, ImageDesc const& desc) :
    RHIResource(device, desc), RHIImage(device, desc), VulkanResource(device, desc), m_views(device.GetAllocator()) {
    // !! TODO
}

VulkanImage::VulkanImage(VulkanDevice const& device, vk::raii::Image&& image, bool shared) :
    RHIResource(device, {}), RHIImage(device, {}), VulkanResource(device, {}), m_image(std::move(image)), m_views(device.GetAllocator()), m_shared(shared) {
    // !! TODO
}

VulkanImage::~VulkanImage() {
    if (m_shared && m_image != nullptr) {
        // If the image is shared (e.g. from a swapchain), we do not destroy it here.
        m_image.release();
    }
    if (m_allocation) {
        auto allocator = m_device.GetVkAllocator();
        vmaDestroyImage(allocator, m_image.release(), m_allocation);
    }
}

RHIImageScopedHandle<RHIImageView> VulkanImage::CreateImageView(RHIImageViewDesc const& desc) {
    auto const& device = m_device.GetVkDevice();
    auto image_view = device.createImageView(
        vk::ImageViewCreateInfo{
            .image = *m_image,
            .viewType = vk::ImageViewType::e2D, // !! TODO
            .format = vkFormatFromRHIFormat(desc.format),
            .subresourceRange = vk::ImageSubresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor, // !! TODO
                .baseMipLevel = desc.base_mip,
                .levelCount = desc.level_count,
                .baseArrayLayer = desc.base_array_layer,
                .layerCount = desc.layer_count
            }
        },
        m_device.GetVkAllocatorCallbacks()
    );
    return { this , m_views.CreateObject<VulkanImageView>(*this, desc, std::move(image_view)) };
}
RHIImageView* VulkanImage::GetImageView(Handle handle) const {
    return m_views.GetObjectPtr<RHIImageView>(handle);
}
void VulkanImage::DestroyImageView(Handle handle) {
    m_views.DestroyObject(handle);
}

VulkanImageView::VulkanImageView(VulkanImage& image, RHIImageViewDesc const& desc, vk::raii::ImageView&& view) :
    RHIImageView(image, desc), m_image(image), m_view(std::move(view)) {}
