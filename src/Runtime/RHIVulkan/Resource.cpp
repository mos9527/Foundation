#include "Resource.hpp"
#include "Device.hpp"
using namespace Foundation;
using namespace Foundation::RHI;
vk::BufferCreateInfo vkBufferCreateInfoFromRHIBufferDesc(RHIBufferDesc const& desc) {
    return vk::BufferCreateInfo{
        .size = desc.size,
        .usage = vkBufferUsageFromRHIBufferUsage(desc.usage),
        .sharingMode = desc.resource.shared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive
    };
}
vk::ImageCreateInfo vkImageCreateInfoFromRHITextureDesc(RHITextureDesc const& desc) {
    vk::ImageType type{};
    using enum RHITextureDimension;
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
    return vk::ImageCreateInfo{
        .imageType = type,
        .format = vkFormatFromRHIFormat(desc.format),
        .extent = vk::Extent3D{ desc.extent.x, desc.extent.y, desc.extent.z },
        .mipLevels = desc.mip_levels,
        .arrayLayers = desc.array_layers,
        .samples = vkSampleCountFlagFromRHIMultisampleCount(desc.sample_count),
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vkImageUsageFlagsFromRHITextureUsage(desc.usage),
        .sharingMode = desc.resource.shared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
        .initialLayout = vkImageLayoutFromRHITextureLayout(desc.initial_layout),
    };
}
VulkanBuffer::VulkanBuffer(VulkanDevice const& device, RHIBufferDesc const& desc)
    : RHIBuffer(device, desc), m_device(device), m_aliases(device.GetAllocator()), m_arena(device.GetAllocator(), desc.size) {
    vk::BufferCreateInfo buffer_info = vkBufferCreateInfoFromRHIBufferDesc(desc);
    VmaAllocationCreateInfo allocInfo = {
        .flags = vmaAllocationFlagsFromRHIResourceHostAccess(desc.resource.host_access),
        .usage = VMA_MEMORY_USAGE_AUTO,
        .requiredFlags = (VkMemoryPropertyFlags)(desc.resource.coherent ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0)
    };
    auto allocator = device.GetVkAllocator();
    VkBuffer buffer;
    auto res = vmaCreateBuffer(allocator,
        &*buffer_info,
        &allocInfo,
        &buffer,
        &m_allocation,
        nullptr
    );
    CHECK(res == VK_SUCCESS && "failed to create Vulkan buffer");
    m_buffer = vk::raii::Buffer(device.GetVkDevice(), vk::Buffer(buffer), device.GetVkAllocatorCallbacks());   
}
VulkanBuffer::VulkanBuffer(VulkanDevice const& device, RHIBufferDesc const& desc, vk::raii::Buffer&& buffer, bool shared)
    : RHIBuffer(device, desc), m_device(device),
    m_buffer(std::move(buffer)), m_aliases(device.GetAllocator()),
    m_shared(shared), m_arena(device.GetAllocator(), desc.size)
{}

VulkanBuffer::~VulkanBuffer() {
    if (m_shared && m_buffer != nullptr) {
        // If the buffer is shared (e.g. from a swapchain), we do not destroy it here.
        m_buffer.release();
    }
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

VulkanTexture::VulkanTexture(VulkanDevice const& device, RHITextureDesc const& desc) :
    RHITexture(device, desc), m_device(device), m_views(device.GetAllocator()), m_shared(false), m_aliases(device.GetAllocator()) {
    vk::ImageType type{};
    using enum RHITextureDimension;
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
    CHECK_MSG(
        desc.extent.x > 0 && desc.extent.y > 0 && desc.extent.z > 0,
        "Extents must be greater than 0. Current x={},y={},z={}",
        desc.extent.x,desc.extent.y,desc.extent.z
    );
    vk::ImageCreateInfo image_info = vkImageCreateInfoFromRHITextureDesc(desc);
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
    CHECK_MSG(res == VK_SUCCESS, "failed to create Vulkan image");
    m_image = vk::raii::Image(device.GetVkDevice(), vk::Image(image), device.GetVkAllocatorCallbacks());    
}

VulkanTexture::VulkanTexture(VulkanDevice const& device, RHITextureDesc const& desc, vk::raii::Image&& image, bool shared) :
    RHITexture(device, desc), m_device(device), m_image(std::move(image)), m_views(device.GetAllocator()), m_shared(shared), m_aliases(device.GetAllocator()) {
}

VulkanTexture::~VulkanTexture() {
    if (m_shared && m_image != nullptr) {
        // If the image is shared (e.g. from a swapchain), we do not destroy it here.
        m_image.release();
    }
    if (m_allocation) {
        auto allocator = m_device.GetVkAllocator();
        vmaDestroyImage(allocator, m_image.release(), m_allocation);
    }
}
void* VulkanTexture::Map() {
    if (!m_mapped)
        vmaMapMemory(m_device.GetVkAllocator(), m_allocation, &m_mapped);
    return m_mapped;
}
void VulkanTexture::Flush(size_t offset, size_t size) {
    if (m_desc.resource.coherent || !m_mapped)
        return;
    if (size == kFullSize)
        size = VK_WHOLE_SIZE;
    vmaFlushAllocation(m_device.GetVkAllocator(), m_allocation, offset, size);
}
void VulkanTexture::Unmap() {
    if (m_mapped)
        vmaUnmapMemory(m_device.GetVkAllocator(), m_allocation);
}
RHITextureScopedHandle<RHITextureView> VulkanTexture::CreateTextureView(RHITextureViewDesc const& desc) {
    auto const& device = m_device.GetVkDevice();
    using enum RHITextureDimension;
    vk::ImageViewType type{};
    switch (desc.dimension)
    {
    case e1D:
        type = vk::ImageViewType::e1D; break;
    case e2D:
        type = vk::ImageViewType::e2D; break;
    case e3D:
    default:
        type = vk::ImageViewType::e3D; break;
    }
    auto image_view = device.createImageView(
        vk::ImageViewCreateInfo{
            .image = *m_image,
            .viewType = type,
            .format = vkFormatFromRHIFormat(desc.format),
            .subresourceRange = vk::ImageSubresourceRange{
                .aspectMask = vkImageAspectFlagFromRHITextureAspect(desc.range.layer.access),
                .baseMipLevel = desc.range.layer.mip_level,
                .levelCount = desc.range.mip_count,
                .baseArrayLayer = desc.range.layer.base_array_layer,
                .layerCount = desc.range.layer.layer_count
            }
        },
        m_device.GetVkAllocatorCallbacks()
    );
    return { this , m_views.CreateObject<VulkanTextureView>(*this, desc, std::move(image_view)) };
}
RHITextureView* VulkanTexture::GetImageView(Handle handle) const {
    return m_views.GetObjectPtr<RHITextureView>(handle);
}
void VulkanTexture::DestroyImageView(Handle handle) {
    m_views.DestroyObject(handle);
}

VulkanTextureView::VulkanTextureView(VulkanTexture& image, RHITextureViewDesc const& desc, vk::raii::ImageView&& view) :
    RHITextureView(image, desc), m_image(image), m_view(std::move(view)) {
}

RHIBufferScopedHandle<RHIBuffer> VulkanBuffer::CreateAliasedBuffer(RHIBufferDesc const& desc, size_t offset) {
    VkBuffer aliased;
    vk::BufferCreateInfo buffer_info = vkBufferCreateInfoFromRHIBufferDesc(desc);
    vmaCreateAliasingBuffer2(m_device.GetVkAllocator(), 
        m_allocation,
        offset,
        &*buffer_info,
        &aliased
    );
    return { this, m_aliases.CreateObject<VulkanBuffer>(m_device, desc, vk::raii::Buffer(m_device.GetVkDevice(), aliased), false /* shared=false */)};
}
RHIBuffer* VulkanBuffer::GetAliasedBuffer(Handle handle) const {
    return m_aliases.GetObjectPtr<RHIBuffer>(handle);
}
void VulkanBuffer::DestroyAliasedBuffer(Handle handle) {
    m_aliases.DestroyObject(handle);
}

void VulkanBuffer::DebugSetObjectName(const char* name) {
    VkBuffer handle = *m_buffer;
    m_device.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eBuffer,
        .objectHandle = (uint64_t)(handle),
        .pObjectName = name
        });
}

RHITextureScopedHandle<RHITexture> VulkanTexture::CreateAliasedTexture(RHITextureDesc const& desc, size_t offset) {
    VkImage aliased;
    vk::ImageCreateInfo image_info = vkImageCreateInfoFromRHITextureDesc(desc);
    vmaCreateAliasingImage2(m_device.GetVkAllocator(), 
        m_allocation,
        offset,
        &*image_info,
        &aliased
    );
    return { this, m_aliases.CreateObject<VulkanTexture>(m_device, desc, vk::raii::Image(m_device.GetVkDevice(), aliased), false /* shared=false */) };
}
RHITexture* VulkanTexture::GetAliasedTexture(Handle handle) const {
    return m_aliases.GetObjectPtr<RHITexture>(handle);
}
void VulkanTexture::DestroyAliasedTexture(Handle handle) {
    m_aliases.DestroyObject(handle);
}

void VulkanTexture::DebugSetObjectName(const char* name) {
    VkImage handle = *m_image;
    m_device.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eImage,
        .objectHandle = (uint64_t)(handle),
        .pObjectName = name
        });
}

void VulkanTextureView::DebugSetObjectName(const char* name) {
    VkImageView handle = *m_view;
    m_image.GetDevice().GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eImageView,
        .objectHandle = (uint64_t)(handle),
        .pObjectName = name
        });
}
