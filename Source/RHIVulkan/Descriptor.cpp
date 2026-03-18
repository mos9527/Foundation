using namespace Foundation;
using namespace Foundation::RHI;
VulkanDeviceDescriptorSet::VulkanDeviceDescriptorSet(VulkanDeviceDescriptorPool const& pool,
                                                     vk::raii::DescriptorSet&& set) :
    RHIDeviceDescriptorSet(pool), mPool(pool), mSet(std::move(set))
{
}

void VulkanDeviceDescriptorSet::Update(UpdateDesc const& desc)
{
    // Sanity check
    size_t size_all = desc.buffers.size() + desc.images.size() + desc.accelerationStructures.size();
    {
        switch (desc.type)
        {
        case RHIDescriptorType::UniformBuffer:
        case RHIDescriptorType::StorageBuffer:
            CHECK_MSG(desc.buffers.size() == size_all,
                      "Buffer descriptor type must have buffers only. Did you set the type?");
            break;
        case RHIDescriptorType::Sampler:
        case RHIDescriptorType::SampledImage:
        case RHIDescriptorType::StorageImage:
            CHECK_MSG(desc.images.size() == size_all,
                      "Image descriptor type must have images only. Did you set the type?");
            break;
        case RHIDescriptorType::AccelerationStructure:
            CHECK_MSG(desc.accelerationStructures.size() == size_all,
                      "AccelerationStructure descriptor type must have AS only. Did you set the type?");
            break;
        default:
            CHECK_MSG(false, "Unsupported or mixed descriptor type");
            break;
        }
    }
    StackArena<> arena;
    AllocatorStack alloc(arena);
    Vector<vk::DescriptorBufferInfo> buffers(desc.buffers.size(), alloc.Ptr());
    for (size_t i = 0; i < desc.buffers.size(); ++i)
    {
        auto const& b = desc.buffers[i];
        buffers[i] = vk::DescriptorBufferInfo{.buffer = static_cast<VulkanBuffer*>(b.buffer)->GetVkBuffer(),
                                              .offset = b.offset,
                                              .range = b.size == kFullSize ? VK_WHOLE_SIZE : b.size};
    }
    Vector<vk::DescriptorImageInfo> images(desc.images.size(), alloc.Ptr());
    for (size_t i = 0; i < desc.images.size(); ++i)
    {
        auto const& img = desc.images[i];
        images[i] = vk::DescriptorImageInfo{
            .sampler = img.sampler ? *static_cast<VulkanDeviceSampler*>(img.sampler)->GetVkSampler() : nullptr,
            .imageView = img.imageView ? *static_cast<VulkanTextureView*>(img.imageView)->GetVkImageView() : nullptr,
            .imageLayout = vkImageLayoutFromRHITextureLayout(img.layout)};
    }
    Vector<vk::AccelerationStructureKHR> as(desc.accelerationStructures.size(), alloc.Ptr());
    for (size_t i = 0; i < as.size(); ++i)
        as[i] =
            static_cast<VulkanAccelerationStructure*>(desc.accelerationStructures[i].as)->GetVkAccelerationStructure();
    vk::WriteDescriptorSet writes{
        .dstSet = *mSet,
        .dstBinding = static_cast<uint32_t>(desc.binding),
        .dstArrayElement = static_cast<uint32_t>(desc.startIndex),
        .descriptorCount = static_cast<uint32_t>(size_all),
        .descriptorType = vkDescriptorTypeFromRHIDescriptorType(desc.type),
        .pImageInfo = images.data(),
        .pBufferInfo = buffers.data(),
    };
    vk::WriteDescriptorSetAccelerationStructureKHR asInfo{
        .accelerationStructureCount = static_cast<uint32_t>(as.size()), .pAccelerationStructures = as.data()};
    if (!as.empty())
        writes.setPNext(&asInfo);
    mPool.GetDevice().GetVkDevice().updateDescriptorSets(writes, {});
}

void VulkanDeviceDescriptorSet::DebugSetObjectName(const char* name)
{
    VkDescriptorSet handle = *mSet;
    mPool.GetDevice().GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eDescriptorSet,
                                                                .objectHandle = reinterpret_cast<uint64_t>(handle),
                                                                .pObjectName = name});
}

VulkanDeviceDescriptorPool::VulkanDeviceDescriptorPool(const VulkanDevice& device, PoolDesc const& desc) :
    RHIDeviceDescriptorPool(device, desc), mDevice(device), mStorage(device.GetAllocator())
{
    StackArena<> arena;
    AllocatorStack alloc(arena);
    Vector<vk::DescriptorPoolSize> pool_sizes(desc.bindings.size(), alloc.Ptr());
    size_t max_sets = 0;
    for (size_t i = 0; i < desc.bindings.size(); ++i)
    {
        auto const& b = desc.bindings[i];
        pool_sizes[i] = vk::DescriptorPoolSize{.type = vkDescriptorTypeFromRHIDescriptorType(b.type),
                                               .descriptorCount = b.maxCount};
        max_sets += b.maxCount;
    }
    vk::DescriptorPoolCreateFlags flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    if (desc.updateAfterBind)
        flags |= vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
    mPool =
        vk::raii::DescriptorPool(mDevice.GetVkDevice(),
                                 vk::DescriptorPoolCreateInfo{.flags = flags,
                                                              .maxSets = static_cast<uint32_t>(max_sets),
                                                              .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
                                                              .pPoolSizes = pool_sizes.data()},
                                 nullptr);
    CHECK(mPool != nullptr && "failed to create Vulkan descriptor pool");
}
RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet>
VulkanDeviceDescriptorPool::CreateDescriptorSet(RHIDeviceHandle<RHIDeviceDescriptorSetLayout> layout,
                                                uint32_t max_variable_count)
{
    auto& vk_layout = layout.Get<VulkanDeviceDescriptorSetLayout>()->GetVkLayout();
    vk::DescriptorSetVariableDescriptorCountAllocateInfo varAlloc{.descriptorSetCount = 1,
                                                                  .pDescriptorCounts = &max_variable_count};
    vk::DescriptorSetAllocateInfo alloc_info{
        .pNext = max_variable_count ? &varAlloc : nullptr,
        .descriptorPool = *mPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &*vk_layout,
    };
    auto set = mDevice.GetVkDevice().allocateDescriptorSets(alloc_info);
    CHECK(!set.empty() && "descriptor set allocation failure");
    auto handle = mStorage.CreateObject<VulkanDeviceDescriptorSet>(*this, std::move(set.front()));
    return {this, handle};
}
RHIDeviceDescriptorSet* VulkanDeviceDescriptorPool::GetDescriptorSet(Handle handle) const
{
    return mStorage.GetObjectPtr<RHIDeviceDescriptorSet>(handle);
}
void VulkanDeviceDescriptorPool::DestroyDescriptorSet(Handle handle) { return mStorage.DestroyObject(handle); }

void VulkanDeviceDescriptorPool::DebugSetObjectName(const char* name)
{
    VkDescriptorPool handle = *mPool;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eDescriptorPool,
                                                      .objectHandle = reinterpret_cast<uint64_t>(handle),
                                                      .pObjectName = name});
}
