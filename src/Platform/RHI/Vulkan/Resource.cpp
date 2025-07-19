#include <Platform/RHI/Vulkan/Resource.hpp>
#include <Platform/RHI/Vulkan/Device.hpp>
using namespace Foundation;
using namespace Foundation::Platform::RHI;

VulkanBuffer::VulkanBuffer(VulkanDevice const& device, RHIBufferDesc const& desc)
    : RHIBuffer(device, desc), m_device(device) {
    vk::BufferCreateInfo buffer_info{
        .size = m_desc.size,
        .usage = vkBufferUsageFromRHIBufferUsage(m_desc.usage),
        .sharingMode = m_desc.resource.shared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive
    };
    VmaAllocationCreateInfo allocInfo = {
        .flags = vmaAllocationFlagsFromRHIResourceHostAccess(desc.resource.host_access),
        .usage = VMA_MEMORY_USAGE_AUTO,
        .requiredFlags = (VkMemoryPropertyFlags)(desc.resource.coherent ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0)
    };
    auto allocator = device.GetVkAllocator();
    VkBuffer buffer;
    auto res =vmaCreateBuffer(allocator,
        &*buffer_info,
        &allocInfo,
        &buffer,
        &m_allocation,
        nullptr
    );
    CHECK(res == VK_SUCCESS && "failed to create Vulkan buffer");
    m_buffer = vk::raii::Buffer(device.GetVkDevice(), vk::Buffer(buffer), device.GetVkAllocatorCallbacks());   
}
VulkanBuffer::~VulkanBuffer() {
    if (m_mapped)
        Unmap();
    if (m_allocation) {
        auto allocator = m_device.GetVkAllocator();
        vmaDestroyBuffer(allocator, m_buffer.release(), m_allocation);
    }
}

void* VulkanBuffer::Map() {
    if (!m_mapped)
        vmaMapMemory(m_device.GetVkAllocator(), m_allocation, &m_mapped);
    return m_mapped;
}
void VulkanBuffer::Flush(size_t offset, size_t size) {
    if (m_desc.resource.coherent || !m_mapped)
        return;
    if (size == kFullSize)
        size = VK_WHOLE_SIZE;
    vmaFlushAllocation(m_device.GetVkAllocator(), m_allocation, offset, size);
}
void VulkanBuffer::Unmap() {
    if (m_mapped)
        vmaUnmapMemory(m_device.GetVkAllocator(), m_allocation);
}

VulkanImage::VulkanImage(VulkanDevice const& device, RHIImageDesc const& desc) :
    RHIImage(device, desc), m_device(device), m_views(device.GetAllocator()), m_shared(false) {
    vk::ImageType type{};
    using enum RHIImageDimension;
    switch (desc.dimension)
    {
    case e1D:
        type = vk::ImageType::e1D; break;
    case e3D:
        type = vk::ImageType::e3D; break;
    default:
    case e2D:
        type = vk::ImageType::e2D; break;
    }
    vk::ImageCreateInfo image_info{
        .imageType = type,
        .format = vkFormatFromRHIFormat(desc.format),
        .extent = vk::Extent3D{ desc.extent.x, desc.extent.y, desc.extent.z },
        .mipLevels = desc.mip_levels,
        .arrayLayers = desc.array_layers,
        .samples = vkSampleCountFlagFromRHIMultisampleCount(desc.sample_count),
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vkImageUsageFlagsFromRHIImageUsage(desc.usage),
        .sharingMode = desc.resource.shared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
        .initialLayout = vkImageLayoutFromRHIImageLayout(desc.initial_layout),
    };
    VmaAllocationCreateInfo allocInfo = {
        .flags = vmaAllocationFlagsFromRHIResourceHostAccess(desc.resource.host_access),
        .usage = VMA_MEMORY_USAGE_AUTO,
        .requiredFlags = (VkMemoryPropertyFlags)(desc.resource.coherent ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0)
    };
    auto allocator = device.GetVkAllocator();
    VkImage image;
    auto res = vmaCreateImage(allocator,
        &*image_info,
        &allocInfo,
        &image,
        &m_allocation,
        nullptr
    );
    CHECK(res == VK_SUCCESS && "failed to create Vulkan image");
    m_image = vk::raii::Image(device.GetVkDevice(), vk::Image(image), device.GetVkAllocatorCallbacks());    
}

VulkanImage::VulkanImage(VulkanDevice const& device, vk::raii::Image&& image, bool shared) :
    RHIImage(device, {}), m_device(device), m_image(std::move(image)), m_views(device.GetAllocator()), m_shared(shared) {
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
void* VulkanImage::Map() {
    if (!m_mapped)
        vmaMapMemory(m_device.GetVkAllocator(), m_allocation, &m_mapped);
    return m_mapped;
}
void VulkanImage::Flush(size_t offset, size_t size) {
    if (m_desc.resource.coherent || !m_mapped)
        return;
    if (size == kFullSize)
        size = VK_WHOLE_SIZE;
    vmaFlushAllocation(m_device.GetVkAllocator(), m_allocation, offset, size);
}
void VulkanImage::Unmap() {
    if (m_mapped)
        vmaUnmapMemory(m_device.GetVkAllocator(), m_allocation);
}
RHIImageScopedHandle<RHIImageView> VulkanImage::CreateImageView(RHIImageViewDesc const& desc) {
    auto const& device = m_device.GetVkDevice();
    using enum RHIImageDimension;
    vk::ImageViewType type{};
    switch (desc.dimension)
    {
    case e1D:
        type = vk::ImageViewType::e1D; break;
    case e3D:
        type = vk::ImageViewType::e3D; break;
    default:
    case e2D:
        type = vk::ImageViewType::e2D; break;
    }
    auto image_view = device.createImageView(
        vk::ImageViewCreateInfo{
            .image = *m_image,
            .viewType = type,
            .format = vkFormatFromRHIFormat(desc.format),
            .subresourceRange = vk::ImageSubresourceRange{
                .aspectMask = vkImageAspectFlagFromRHIImageAspect(desc.range.layer.access), // !! TODO
                .baseMipLevel = desc.range.layer.mip_level,
                .levelCount = desc.range.mip_count,
                .baseArrayLayer = desc.range.layer.base_array_layer,
                .layerCount = desc.range.layer.layer_count
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
    RHIImageView(image, desc), m_image(image), m_view(std::move(view)) {
}
