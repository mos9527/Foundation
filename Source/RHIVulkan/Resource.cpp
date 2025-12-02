using namespace Foundation;
using namespace Foundation::RHI;
VmaMemoryUsage vmaMemoryUsageFlagsFromResource(RHIResourceDesc const& desc)
{
    switch (desc.heap)
    {
    case RHIDeviceHeapType::Local:
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    case RHIDeviceHeapType::Readback:
    case RHIDeviceHeapType::Upload:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    }
}
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
    case E1D:
        type = vk::ImageType::e1D; break;
    case E3D:
        type = vk::ImageType::e3D; break;
    default:
    case E2D:
        type = vk::ImageType::e2D; break;
    }
    return vk::ImageCreateInfo{
        .imageType = type,
        .format = vkFormatFromRHIFormat(desc.format),
        .extent = vk::Extent3D{ desc.extent.x, desc.extent.y, desc.extent.z },
        .mipLevels = desc.mipLevels,
        .arrayLayers = desc.arrayLayers,
        .samples = vkSampleCountFlagFromRHIMultisampleCount(desc.sampleCount),
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vkImageUsageFlagsFromRHITextureUsage(desc.usage),
        .sharingMode = desc.resource.shared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
        .initialLayout = vkImageLayoutFromRHITextureLayout(desc.initialLayout),
    };
}
uint32_t FillQueueFamilies(VulkanDevice const& device, uint32_t* dst, RHIDeviceQueueFlags queueFlags)
{
    Bitset<256> uniqueFamilies{};
    if (queueFlags & RHIDeviceQueueFlagsBits::Graphics)
        uniqueFamilies[device.GetDeviceQueue(RHIDeviceQueueType::Graphics)->GetVkQueueFamily()] = true;
    if (queueFlags & RHIDeviceQueueFlagsBits::Compute)
        uniqueFamilies[device.GetDeviceQueue(RHIDeviceQueueType::Compute)->GetVkQueueFamily()] = true;
    if (queueFlags & RHIDeviceQueueFlagsBits::Transfer)
        uniqueFamilies[device.GetDeviceQueue(RHIDeviceQueueType::Transfer)->GetVkQueueFamily()] = true;
    uint32_t* p = dst;
    for (size_t i = 0; i < uniqueFamilies.size(); ++i)
    {
        if (uniqueFamilies[i])
            *p++ = static_cast<uint32_t>(i);
    }
    return p - dst;
}
VulkanBuffer::VulkanBuffer(VulkanDevice const& device, RHIBufferDesc const& desc)
    : RHIBuffer(device, desc), mDevice(device), mAliases(device.GetAllocator()) {
    vk::BufferCreateInfo buffer_info = vkBufferCreateInfoFromRHIBufferDesc(desc);

    uint32_t queueFamilies[8]{};
    if (desc.resource.shared)
    {
        buffer_info.pQueueFamilyIndices = queueFamilies;
        buffer_info.queueFamilyIndexCount = FillQueueFamilies(device, &queueFamilies[0], desc.resource.sharedQueues);
    }

    auto flags = vmaAllocationFlagsFromRHIResourceHostAccess(desc.resource.hostAccess);
    if (desc.resource.staging)
        flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT,
        flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    VmaAllocationCreateInfo allocInfo = {
        .flags = flags,
        .usage = vmaMemoryUsageFlagsFromResource(desc.resource),
        .requiredFlags = static_cast<VkMemoryPropertyFlags>(desc.resource.coherent ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0)
    };
    auto allocator = device.GetVkAllocator();
    VkBuffer buffer;
    auto res = vmaCreateBuffer(allocator,
        &*buffer_info,
        &allocInfo,
        &buffer,
        &mAllocation,
        nullptr
    );
    CHECK(res == VK_SUCCESS && "failed to create Vulkan buffer");
    mBuffer = vk::raii::Buffer(device.GetVkDevice(), vk::Buffer(buffer), device.GetVkAllocatorCallbacks());   
}
VulkanBuffer::VulkanBuffer(VulkanDevice const& device, RHIBufferDesc const& desc, vk::raii::Buffer&& buffer, bool shared)
    : RHIBuffer(device, desc), mDevice(device),
    mBuffer(std::move(buffer)), mAliases(device.GetAllocator()), mShared(shared)
{}

VulkanBuffer::~VulkanBuffer() {
    if (mShared && mBuffer != nullptr) {
        // If the buffer is shared (e.g. from a swapchain), we do not destroy it here.
        mBuffer.release();
    }
    if (mMapped)
        VulkanBuffer::Unmap();
    if (mAllocation) {
        auto allocator = mDevice.GetVkAllocator();
        vmaDestroyBuffer(allocator, mBuffer.release(), mAllocation);
    }
}

void* VulkanBuffer::Map() {
    if (!mMapped)
        vmaMapMemory(mDevice.GetVkAllocator(), mAllocation, &mMapped);
    return mMapped;
}
void VulkanBuffer::Flush(size_t offset, size_t size) {
    if (mDesc.resource.coherent || !mMapped)
        return;
    if (size == kFullSize)
        size = VK_WHOLE_SIZE;
    vmaFlushAllocation(mDevice.GetVkAllocator(), mAllocation, offset, size);
}
void VulkanBuffer::Unmap() {
    if (mMapped)
        vmaUnmapMemory(mDevice.GetVkAllocator(), mAllocation);
}

VulkanTexture::VulkanTexture(VulkanDevice const& device, RHITextureDesc const& desc) :
    RHITexture(device, desc), mDevice(device), mAliases(device.GetAllocator()), mViews(device.GetAllocator()), mShared(false) {
    CHECK_MSG(
        desc.extent.x > 0 && desc.extent.y > 0 && desc.extent.z > 0,
        "Extents must be greater than 0. Current x={},y={},z={}",
        desc.extent.x,desc.extent.y,desc.extent.z
    );
    vk::ImageCreateInfo image_info = vkImageCreateInfoFromRHITextureDesc(desc);
    uint32_t queueFamilies[8]{};
    if (desc.resource.shared)
    {
        image_info.pQueueFamilyIndices = queueFamilies;
        image_info.queueFamilyIndexCount = FillQueueFamilies(device, &queueFamilies[0], desc.resource.sharedQueues);
    }
    VmaAllocationCreateInfo allocInfo = {
        .flags = vmaAllocationFlagsFromRHIResourceHostAccess(desc.resource.hostAccess),
        .usage = vmaMemoryUsageFlagsFromResource(desc.resource),
        .requiredFlags = static_cast<VkMemoryPropertyFlags>(desc.resource.coherent ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0)
    };
    auto allocator = device.GetVkAllocator();
    VkImage image;
    auto res = vmaCreateImage(allocator,
        &*image_info,
        &allocInfo,
        &image,
        &mAllocation,
        nullptr
    );
    CHECK_MSG(res == VK_SUCCESS, "failed to create Vulkan image");
    mImage = vk::raii::Image(device.GetVkDevice(), vk::Image(image), device.GetVkAllocatorCallbacks());    
}

VulkanTexture::VulkanTexture(VulkanDevice const& device, RHITextureDesc const& desc, vk::raii::Image&& image, bool shared) :
    RHITexture(device, desc), mDevice(device), mImage(std::move(image)), mAliases(device.GetAllocator()), mViews(device.GetAllocator()), mShared(shared) {
}

VulkanTexture::~VulkanTexture() {
    if (mShared && mImage != nullptr) {
        // If the image is shared (e.g. from a swapchain), we do not destroy it here.
        mImage.release();
    }
    if (mAllocation) {
        auto allocator = mDevice.GetVkAllocator();
        vmaDestroyImage(allocator, mImage.release(), mAllocation);
    }
}

RHITextureScopedHandle<RHITextureView> VulkanTexture::CreateTextureView(RHITextureViewDesc const& desc) {
    auto const& device = mDevice.GetVkDevice();
    using enum RHITextureDimension;
    vk::ImageViewType type{};
    switch (desc.dimension)
    {
    case E1D:
        type = vk::ImageViewType::e1D; break;
    case E2D:
        type = vk::ImageViewType::e2D; break;
    case E3D:
    default:
        type = vk::ImageViewType::e3D; break;
    }
    auto image_view = device.createImageView(
        vk::ImageViewCreateInfo{
            .image = *mImage,
            .viewType = type,
            .format = vkFormatFromRHIFormat(desc.format),
            .subresourceRange = vk::ImageSubresourceRange{
                .aspectMask = vkImageAspectFlagFromRHITextureAspect(desc.range.layer.aspect),
                .baseMipLevel = desc.range.layer.mipLevel,
                .levelCount = desc.range.mipCount,
                .baseArrayLayer = desc.range.layer.baseArrayLayer,
                .layerCount = desc.range.layer.layerCount
            }
        },
        mDevice.GetVkAllocatorCallbacks()
    );
    return { this , mViews.CreateObject<VulkanTextureView>(*this, desc, std::move(image_view)) };
}
RHITextureView* VulkanTexture::GetImageView(Handle handle) const {
    return mViews.GetObjectPtr<RHITextureView>(handle);
}
void VulkanTexture::DestroyImageView(Handle handle) {
    mViews.DestroyObject(handle);
}

VulkanTextureView::VulkanTextureView(VulkanTexture& image, RHITextureViewDesc const& desc, vk::raii::ImageView&& view) :
    RHITextureView(image, desc), mView(std::move(view)), mImage(image) {
}

RHIBufferScopedHandle<RHIBuffer> VulkanBuffer::CreateAliasedBuffer(RHIBufferDesc const& desc, size_t offset) {
    VkBuffer aliased;
    vk::BufferCreateInfo buffer_info = vkBufferCreateInfoFromRHIBufferDesc(desc);
    vmaCreateAliasingBuffer2(mDevice.GetVkAllocator(), 
        mAllocation,
        offset,
        &*buffer_info,
        &aliased
    );
    return { this, mAliases.CreateObject<VulkanBuffer>(mDevice, desc, vk::raii::Buffer(mDevice.GetVkDevice(), aliased), false /* shared=false */)};
}
RHIBuffer* VulkanBuffer::GetAliasedBuffer(Handle handle) const {
    return mAliases.GetObjectPtr<RHIBuffer>(handle);
}
void VulkanBuffer::DestroyAliasedBuffer(Handle handle) {
    mAliases.DestroyObject(handle);
}

void VulkanBuffer::DebugSetObjectName(const char* name) {
    VkBuffer handle = *mBuffer;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eBuffer,
        .objectHandle = reinterpret_cast<uint64_t>(handle),
        .pObjectName = name
        });
}

RHITextureScopedHandle<RHITexture> VulkanTexture::CreateAliasedTexture(RHITextureDesc const& desc, size_t offset) {
    VkImage aliased;
    vk::ImageCreateInfo image_info = vkImageCreateInfoFromRHITextureDesc(desc);
    vmaCreateAliasingImage2(mDevice.GetVkAllocator(), 
        mAllocation,
        offset,
        &*image_info,
        &aliased
    );
    return { this, mAliases.CreateObject<VulkanTexture>(mDevice, desc, vk::raii::Image(mDevice.GetVkDevice(), aliased), false /* shared=false */) };
}
RHITexture* VulkanTexture::GetAliasedTexture(Handle handle) const {
    return mAliases.GetObjectPtr<RHITexture>(handle);
}
void VulkanTexture::DestroyAliasedTexture(Handle handle) {
    mAliases.DestroyObject(handle);
}

void VulkanTexture::DebugSetObjectName(const char* name) {
    VkImage handle = *mImage;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eImage,
        .objectHandle = reinterpret_cast<uint64_t>(handle),
        .pObjectName = name
        });
}

void VulkanTextureView::DebugSetObjectName(const char* name) {
    VkImageView handle = *mView;
    mImage.GetDevice().GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eImageView,
        .objectHandle = reinterpret_cast<uint64_t>(handle),
        .pObjectName = name
        });
}
