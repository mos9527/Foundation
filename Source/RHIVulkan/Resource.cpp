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
    default:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    }
}
vk::BufferCreateInfo vkBufferCreateInfoFromRHIBufferDesc(RHIBufferDesc const& desc)
{
    return vk::BufferCreateInfo{.size = desc.size,
                                .usage = vkBufferUsageFromRHIBufferUsage(desc.usage),
                                .sharingMode =
                                    desc.resource.shared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive};
}
vk::ImageCreateInfo vkImageCreateInfoFromRHITextureDesc(RHITextureDesc const& desc)
{
    vk::ImageType type{};
    using enum RHITextureDimension;
    switch (desc.dimension)
    {
    case E1D:
        type = vk::ImageType::e1D;
        break;
    case E3D:
        type = vk::ImageType::e3D;
        break;
    default:
    case E2D:
        type = vk::ImageType::e2D;
        break;
    }
    return vk::ImageCreateInfo{
        .imageType = type,
        .format = vkFormatFromRHIFormat(desc.format),
        .extent = vk::Extent3D{desc.extent.x, desc.extent.y, desc.extent.z},
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
VulkanBuffer::VulkanBuffer(VulkanDevice const& device, RHIBufferDesc const& desc) :
    RHIBuffer(device, desc), mDevice(device), mAliases(device.GetAllocator())
{
    vk::BufferCreateInfo bufferInfo = vkBufferCreateInfoFromRHIBufferDesc(desc);

    uint32_t queueFamilies[8]{};
    if (desc.resource.shared)
    {
        bufferInfo.pQueueFamilyIndices = queueFamilies;
        bufferInfo.queueFamilyIndexCount = FillQueueFamilies(device, &queueFamilies[0], desc.resource.sharedQueues);
        // Fallback. VVL complians otherwise.
        if (bufferInfo.queueFamilyIndexCount < 2)
            bufferInfo.sharingMode = vk::SharingMode::eExclusive;
    }

    auto flags = vmaAllocationFlagsFromRHIResourceHostAccess(desc.resource.hostAccess);
    if (desc.resource.staging)
        flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    VkMemoryPropertyFlags requiredFlags = desc.resource.coherent ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0;
    if (desc.resource.heap == RHIDeviceHeapType::Local && desc.resource.hostAccess != RHIResourceHostAccess::Invisible)
        requiredFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    VmaAllocationCreateInfo allocInfo = {.flags = flags,
                                         .usage = vmaMemoryUsageFlagsFromResource(desc.resource),
                                         .requiredFlags = requiredFlags};
    auto allocator = device.GetVkAllocator();
    VkBuffer buffer;
    auto res = vmaCreateBufferWithAlignment(allocator, reinterpret_cast<const VkBufferCreateInfo*>(&bufferInfo), &allocInfo, desc.alignment, &buffer, &mAllocation,
                                            nullptr);
    CHECK(res == VK_SUCCESS && "failed to create Vulkan buffer");
    mBuffer = vk::raii::Buffer(device.GetVkDevice(), vk::Buffer(buffer), device.GetVkAllocationCallbacks());
}
VulkanBuffer::VulkanBuffer(VulkanDevice const& device, RHIBufferDesc const& desc, vk::raii::Buffer&& buffer,
                           bool shared) :
    RHIBuffer(device, desc), mDevice(device), mBuffer(std::move(buffer)), mAliases(device.GetAllocator()),
    mShared(shared)
{
}

VulkanBuffer::~VulkanBuffer()
{
    if (mShared && *mBuffer)
    {
        // If the buffer is shared (e.g. from a swapchain), we do not destroy it here.
        mBuffer.release();
    }
    if (mMapped)
        VulkanBuffer::Unmap();
    if (mAllocation)
    {
        auto allocator = mDevice.GetVkAllocator();
        vmaDestroyBuffer(allocator, mBuffer.release(), mAllocation);
    }
}

void* VulkanBuffer::Map()
{
    if (!mMapped)
        vmaMapMemory(mDevice.GetVkAllocator(), mAllocation, &mMapped);
    return mMapped;
}
void VulkanBuffer::Flush(size_t offset, size_t size)
{
    if (mDesc.resource.coherent || !mMapped)
        return;
    if (size == kFullSize)
        size = VK_WHOLE_SIZE;
    vmaFlushAllocation(mDevice.GetVkAllocator(), mAllocation, offset, size);
}
void VulkanBuffer::Unmap()
{
    if (mMapped)
        vmaUnmapMemory(mDevice.GetVkAllocator(), mAllocation);
    mMapped = nullptr;
}
size_t VulkanBuffer::GetAllocationSize() const
{
    if (!mAllocation)
        return 0;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(mDevice.GetVkAllocator(), mAllocation, &info);
    return static_cast<size_t>(info.size);
}
vk::DeviceAddress VulkanBuffer::GetBufferAddress() const
{
    return mDevice.GetVkDevice().getBufferAddress({.buffer = mBuffer});
}

VulkanTexture::VulkanTexture(VulkanDevice const& device, RHITextureDesc const& desc) :
    RHITexture(device, desc), mDevice(device), mAliases(device.GetAllocator()), mViews(device.GetAllocator()),
    mShared(false)
{
    CHECK_MSG(desc.extent.x > 0 && desc.extent.y > 0 && desc.extent.z > 0,
              "Extents must be greater than 0. Current x={},y={},z={}", desc.extent.x, desc.extent.y, desc.extent.z);
    vk::ImageCreateInfo imageInfo = vkImageCreateInfoFromRHITextureDesc(desc);
    uint32_t queueFamilies[8]{};
    if (desc.resource.shared)
    {
        imageInfo.pQueueFamilyIndices = queueFamilies;
        imageInfo.queueFamilyIndexCount = FillQueueFamilies(device, &queueFamilies[0], desc.resource.sharedQueues);
        // Fallback. VVL complians otherwise.
        if (imageInfo.queueFamilyIndexCount < 2)
            imageInfo.sharingMode = vk::SharingMode::eExclusive;
    }
    VmaAllocationCreateInfo allocInfo = {.flags = vmaAllocationFlagsFromRHIResourceHostAccess(desc.resource.hostAccess),
                                         .usage = vmaMemoryUsageFlagsFromResource(desc.resource),
                                         .requiredFlags = static_cast<VkMemoryPropertyFlags>(
                                             desc.resource.coherent ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0)};
    auto allocator = device.GetVkAllocator();
    VkImage image;
    auto res = vmaCreateImage(allocator, reinterpret_cast<const VkImageCreateInfo*>(&imageInfo), &allocInfo, &image, &mAllocation, nullptr);
    CHECK_MSG(res == VK_SUCCESS, "failed to create Vulkan image");
    mImage = vk::raii::Image(device.GetVkDevice(), vk::Image(image), device.GetVkAllocationCallbacks());
}

VulkanTexture::VulkanTexture(VulkanDevice const& device, RHITextureDesc const& desc, vk::raii::Image&& image,
                             bool shared) :
    RHITexture(device, desc), mDevice(device), mImage(std::move(image)), mAliases(device.GetAllocator()),
    mViews(device.GetAllocator()), mShared(shared)
{
}

VulkanTexture::~VulkanTexture()
{
    if (mShared && *mImage)
    {
        // If the image is shared (e.g. from a swapchain), we do not destroy it here.
        mImage.release();
    }
    if (mAllocation)
    {
        auto allocator = mDevice.GetVkAllocator();
        vmaDestroyImage(allocator, mImage.release(), mAllocation);
    }
}
size_t VulkanTexture::GetAllocationSize() const
{
    if (!mAllocation)
        return 0;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(mDevice.GetVkAllocator(), mAllocation, &info);
    return static_cast<size_t>(info.size);
}

RHITextureScopedHandle<RHITextureView> VulkanTexture::CreateTextureView(RHITextureViewDesc const& desc)
{
    auto const& device = mDevice.GetVkDevice();
    using enum RHITextureDimension;
    vk::ImageViewType type{};
    switch (desc.dimension)
    {
    case E1D:
        type = vk::ImageViewType::e1D;
        break;
    case E2D:
        type = vk::ImageViewType::e2D;
        break;
    case E3D:
        type = vk::ImageViewType::e3D;
        break;
    case ECube:
        type = vk::ImageViewType::eCube;
        break;
    case E1DArray:
        type = vk::ImageViewType::e1DArray;
        break;
    case E2DArray:
        type = vk::ImageViewType::e2DArray;
        break;
    case ECubeArray:
        type = vk::ImageViewType::eCubeArray;
    }
    auto image_view = device.createImageView(
        vk::ImageViewCreateInfo{
            .image = *mImage,
            .viewType = type,
            .format = vkFormatFromRHIFormat(desc.format),
            .subresourceRange =
                vk::ImageSubresourceRange{.aspectMask = vkImageAspectFlagFromRHITextureAspect(desc.range.layer.aspect),
                                          .baseMipLevel = desc.range.layer.mipLevel,
                                          .levelCount = desc.range.mipCount,
                                          .baseArrayLayer = desc.range.layer.baseArrayLayer,
                                          .layerCount = desc.range.layer.layerCount}},
        mDevice.GetVkAllocationCallbacks());
    return {this, mViews.CreateObject<VulkanTextureView>(*this, desc, std::move(image_view))};
}
RHITextureView* VulkanTexture::GetImageView(Handle handle) const { return mViews.GetObjectPtr<RHITextureView>(handle); }
void VulkanTexture::DestroyImageView(Handle handle) { mViews.DestroyObject(handle); }

VulkanTextureView::VulkanTextureView(VulkanTexture& image, RHITextureViewDesc const& desc, vk::raii::ImageView&& view) :
    RHITextureView(image, desc), mView(std::move(view)), mImage(image)
{
}

RHIBufferScopedHandle<RHIBuffer> VulkanBuffer::CreateAliasedBuffer(RHIBufferDesc const& desc, size_t offset)
{
    VkBuffer aliased;
    vk::BufferCreateInfo buffer_info = vkBufferCreateInfoFromRHIBufferDesc(desc);
    vmaCreateAliasingBuffer2(mDevice.GetVkAllocator(), mAllocation, offset, reinterpret_cast<const VkBufferCreateInfo*>(&buffer_info), &aliased);
    return {this,
            mAliases.CreateObject<VulkanBuffer>(mDevice, desc,
                                                vk::raii::Buffer(mDevice.GetVkDevice(), aliased,
                                                                 mDevice.GetVkAllocationCallbacks()),
                                                false /* shared=false */)};
}
RHIBuffer* VulkanBuffer::GetAliasedBuffer(Handle handle) const { return mAliases.GetObjectPtr<RHIBuffer>(handle); }
void VulkanBuffer::DestroyAliasedBuffer(Handle handle) { mAliases.DestroyObject(handle); }

void VulkanBuffer::DebugSetObjectName(const char* name)
{
    VkBuffer handle = *mBuffer;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eBuffer,
                                                      .objectHandle = reinterpret_cast<uint64_t>(handle),
                                                      .pObjectName = name});
}

RHITextureScopedHandle<RHITexture> VulkanTexture::CreateAliasedTexture(RHITextureDesc const& desc, size_t offset)
{
    VkImage aliased;
    vk::ImageCreateInfo image_info = vkImageCreateInfoFromRHITextureDesc(desc);
    vmaCreateAliasingImage2(mDevice.GetVkAllocator(), mAllocation, offset, reinterpret_cast<const VkImageCreateInfo*>(&image_info), &aliased);
    return {this,
            mAliases.CreateObject<VulkanTexture>(mDevice, desc,
                                                 vk::raii::Image(mDevice.GetVkDevice(), aliased,
                                                                 mDevice.GetVkAllocationCallbacks()),
                                                 false /* shared=false */)};
}
RHITexture* VulkanTexture::GetAliasedTexture(Handle handle) const { return mAliases.GetObjectPtr<RHITexture>(handle); }
void VulkanTexture::DestroyAliasedTexture(Handle handle) { mAliases.DestroyObject(handle); }

void VulkanTexture::DebugSetObjectName(const char* name)
{
    VkImage handle = *mImage;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eImage,
                                                      .objectHandle = reinterpret_cast<uint64_t>(handle),
                                                      .pObjectName = name});
}

void VulkanTextureView::DebugSetObjectName(const char* name)
{
    VkImageView handle = *mView;
    mImage.GetDevice().GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eImageView,
                                                                 .objectHandle = reinterpret_cast<uint64_t>(handle),
                                                                 .pObjectName = name});
}
VulkanAccelerationStructure::VulkanAccelerationStructure(VulkanDevice const& device,
                                                         RHIAccelerationStructureDesc const& desc) :
    RHIAccelerationStructure(device, desc), mDevice(device)
{
    mBuffer = static_cast<VulkanBuffer*>(desc.buffer);
    mAS = device.GetVkDevice().createAccelerationStructureKHR(
        vk::AccelerationStructureCreateInfoKHR{
            .buffer = mBuffer->GetVkBuffer(),
            .offset = desc.offset,
            .size = desc.size,
            .type = desc.type == RHIAccelerationStructureType::TopLevel
                ? vk::AccelerationStructureTypeKHR::eTopLevel
                : vk::AccelerationStructureTypeKHR::eBottomLevel},
        device.GetVkAllocationCallbacks());
    mASAddress = mDevice.GetVkDevice().getAccelerationStructureAddressKHR({.accelerationStructure = mAS});
}
vk::DeviceAddress VulkanAccelerationStructure::GetVkAccelerationStructureAddress() const
{
    return mASAddress;
}
void VulkanAccelerationStructure::DebugSetObjectName(const char* name) { 
    VkAccelerationStructureKHR handle = *mAS;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eAccelerationStructureKHR,
        .objectHandle = reinterpret_cast<uint64_t>(handle),
		.pObjectName = name});
}

vk::AccelerationStructureGeometryTrianglesDataKHR
RHI::vkAccelerationTriangleDataFromRHI(RHIAccelerationStructureGeometryTriangleData const& src)
{
    auto vbuf = static_cast<VulkanBuffer*>(src.vertexBuffer)->GetBufferAddress() + src.vertexOffset;
    auto ibuf = static_cast<VulkanBuffer*>(src.indexBuffer)->GetBufferAddress() + src.indexOffset;
    CHECK(src.indexFormat == RHIResourceFormat::R32Uint || src.indexFormat == RHIResourceFormat::R16Uint);
    return vk::AccelerationStructureGeometryTrianglesDataKHR{
        .vertexFormat = vkFormatFromRHIFormat(src.vertexFormat),
        .vertexData = vk::DeviceOrHostAddressConstKHR{.deviceAddress = vbuf},
        .vertexStride = src.vertexStride,
        .maxVertex = src.vertexCount,
        .indexType = src.indexFormat == RHIResourceFormat::R32Uint ? vk::IndexType::eUint32 : vk::IndexType::eUint16,
        .indexData = vk::DeviceOrHostAddressConstKHR{.deviceAddress = ibuf},
    };
}

vk::AccelerationStructureGeometryAabbsDataKHR
RHI::vkAccelerationAABBDataFromRHI(RHIAccelerationStructureGeometryAABBData const& src)
{
    auto* buf = static_cast<VulkanBuffer*>(src.aabbBuffer);
    auto addr = buf->GetBufferAddress() + src.offset;
    CHECK(src.count > 0);
    CHECK(src.offset < buf->mDesc.size);
    CHECK(src.stride >= sizeof(RHIAccelerationStructureAABB));
    CHECK(src.offset + static_cast<uint64_t>(src.count) * src.stride <= buf->mDesc.size);
    return vk::AccelerationStructureGeometryAabbsDataKHR{
        .data = vk::DeviceOrHostAddressConstKHR{.deviceAddress = addr},
        .stride = src.stride,
    };
}

vk::AccelerationStructureBuildGeometryInfoKHR
RHI::vkAccelerationBuildGeoInfoFromRHI(RHIAccelerationStructureBuildDesc const& desc,
                                       Vector<vk::AccelerationStructureGeometryKHR>& geometries,
                                       Vector<uint32_t>& primitiveCounts)
{
    geometries.resize(desc.geometries.size()), primitiveCounts.clear();
    for (size_t i = 0; auto const& src : desc.geometries)
    {
        auto& geo = geometries[i];
        switch (src.type)
        {
        case RHIAccelerationGeometryType::Triangles:
            {
                CHECK(src.triangleData.indexCount && src.triangleData.indexCount % 3 == 0);
                geo.geometryType = vk::GeometryTypeKHR::eTriangles;
                // vvv Unions don't initialize these themselves.
                geo.geometry.triangles.sType = vk::StructureType::eAccelerationStructureGeometryTrianglesDataKHR;
                geo.geometry.triangles = vkAccelerationTriangleDataFromRHI(src.triangleData);
                primitiveCounts.push_back(src.triangleData.indexCount / 3);
                break;
            }
        case RHIAccelerationGeometryType::AABBs:
            {
                geo.geometryType = vk::GeometryTypeKHR::eAabbs;
                geo.geometry.aabbs.sType = vk::StructureType::eAccelerationStructureGeometryAabbsDataKHR;
                geo.geometry.aabbs = vkAccelerationAABBDataFromRHI(src.aabbData);
                primitiveCounts.push_back(src.aabbData.count);
                break;
            }
        case RHIAccelerationGeometryType::Instances:
            {
                auto* buf = static_cast<VulkanBuffer*>(src.instanceData.instanceBuffer);
                auto addr = buf->GetBufferAddress();
                CHECK(src.instanceData.instanceOffset < buf->mDesc.size);
                geo.geometryType = vk::GeometryTypeKHR::eInstances;
                geo.geometry.instances.sType = vk::StructureType::eAccelerationStructureGeometryInstancesDataKHR;
                geo.geometry.instances.data = {.deviceAddress = addr + src.instanceData.instanceOffset};
                primitiveCounts.push_back(src.instanceData.totalPrimitives);
                break;
            }
        }
        i++;
    }
    auto res = vk::AccelerationStructureBuildGeometryInfoKHR{
        .type = desc.type == RHIAccelerationStructureType::TopLevel ? vk::AccelerationStructureTypeKHR::eTopLevel
                                                                    : vk::AccelerationStructureTypeKHR::eBottomLevel,
        .flags = vkBuildAccelerationStructureFlagsFromRHIAccelerationStructureBuildFlags(desc.flags),
        .mode = desc.operation == RHIAccelerationStructureBuildOp::Build
            ? vk::BuildAccelerationStructureModeKHR::eBuild
            : vk::BuildAccelerationStructureModeKHR::eUpdate,
        .geometryCount = static_cast<uint32_t>(geometries.size()),
        .pGeometries = geometries.data(),
    };
    if (desc.scratchBuffer)
    {
        auto* vkBuffer = static_cast<VulkanBuffer*>(desc.scratchBuffer);
        res.scratchData = {.deviceAddress = vkBuffer->GetBufferAddress() + desc.scratchBufferOffset};
    }
    if (desc.srcAS)
        res.srcAccelerationStructure =
            static_cast<VulkanAccelerationStructure*>(desc.srcAS)->GetVkAccelerationStructure();
    if (desc.dstAS)
        res.dstAccelerationStructure =
            static_cast<VulkanAccelerationStructure*>(desc.dstAS)->GetVkAccelerationStructure();
    return res;
}
vk::AccelerationStructureBuildRangeInfoKHR
RHI::vkAccelerationBuildRangeInfoFromRHI(RHIAccelerationStructureBuildRangeInfo const& desc)
{
    return vk::AccelerationStructureBuildRangeInfoKHR{
        .primitiveCount = desc.primitiveCount,
        .primitiveOffset = desc.offset,
        .firstVertex = desc.firstVertex,
        .transformOffset = desc.transformOffset,
    };
}
