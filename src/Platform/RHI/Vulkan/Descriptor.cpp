#include <Core/Allocator/StackAllocator.hpp>
#include <Platform/RHI/Vulkan/Device.hpp>
#include <Platform/RHI/Vulkan/Descriptor.hpp>
#include <Platform/RHI/Vulkan/Resource.hpp>
using namespace Foundation;
using namespace Foundation::Platform::RHI;
VulkanDeviceDescriptorSet::VulkanDeviceDescriptorSet(VulkanDeviceDescriptorPool const& pool, vk::raii::DescriptorSet&& set) :
    RHIDeviceDescriptorSet(pool), m_pool(pool), m_set(std::move(set)) {
}

void VulkanDeviceDescriptorSet::Update(UpdateDesc const& desc)
{
    Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
    Core::StlVector<vk::DescriptorBufferInfo> buffers(desc.buffers.size(), alloc.Ptr());
    for (size_t i = 0; i < desc.buffers.size(); ++i) {
        auto const& b = desc.buffers[i];
        buffers[i] = vk::DescriptorBufferInfo{
            .buffer = static_cast<VulkanBuffer*>(b.buffer)->GetVkBuffer(),
            .offset = b.offset,
            .range = b.size == kFullSize ? VK_WHOLE_SIZE : b.size
        };
    }
    vk::WriteDescriptorSet writes{
        .dstSet = *m_set,
        .dstBinding = static_cast<uint32_t>(desc.binding),
        .descriptorCount = static_cast<uint32_t>(desc.buffers.size()),
        .descriptorType = vkDescriptorTypeFromRHIDescriptorType(desc.type),
        .pBufferInfo = buffers.data()   
    };
    m_pool.GetDevice().GetVkDevice().updateDescriptorSets(writes, {});
}

VulkanDeviceDescriptorPool::VulkanDeviceDescriptorPool(const VulkanDevice& device, PoolDesc const& desc)
    : RHIDeviceDescriptorPool(device, desc), m_device(device), m_storage(device.GetAllocator()) {
    Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
    Core::StlVector<vk::DescriptorPoolSize> pool_sizes(desc.bindings.size(), alloc.Ptr());
    size_t max_sets = 0;
    for (size_t i = 0; i < desc.bindings.size(); ++i) {
        auto const& b = desc.bindings[i];
        pool_sizes[i] = vk::DescriptorPoolSize{
            .type = vkDescriptorTypeFromRHIDescriptorType(b.type),
            .descriptorCount = b.max_count
        };
        max_sets += b.max_count;
    }
    m_pool = vk::raii::DescriptorPool(
        m_device.GetVkDevice(),
        vk::DescriptorPoolCreateInfo{
            .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
            .maxSets = static_cast<uint32_t>(max_sets),
            .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
            .pPoolSizes = pool_sizes.data()
        },
        m_device.GetVkAllocatorCallbacks()
    );
}
RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet> VulkanDeviceDescriptorPool::CreateDescriptorSet(
    RHIDeviceObjectHandle<RHIDeviceDescriptorSetLayout> layout) {
    auto& vk_layout = layout.Get<VulkanDeviceDescriptorSetLayout>()->GetVkLayout();
    vk::DescriptorSetAllocateInfo alloc_info{
        .descriptorPool = *m_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &*vk_layout
    };
    auto set = m_device.GetVkDevice().allocateDescriptorSets(alloc_info);
    CHECK(!set.empty() && "descriptor set allocation failure");
    auto handle = m_storage.CreateObject<VulkanDeviceDescriptorSet>(*this, std::move(set.front()));
    return { this, handle };
}
RHIDeviceDescriptorSet* VulkanDeviceDescriptorPool::GetDescriptorSet(Handle handle) const {
    return m_storage.GetObjectPtr<RHIDeviceDescriptorSet>(handle);
}
void VulkanDeviceDescriptorPool::DestroyDescriptorSet(Handle handle) {
    return m_storage.DestroyObject(handle);
}
