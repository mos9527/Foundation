#pragma once
#include <RHICore/Descriptor.hpp>
#include "Common.hpp"

namespace Foundation::RHI {
    class VulkanDevice;
    class VulkanDeviceDescriptorPool;
    class VulkanDeviceDescriptorSetLayout;
    class VulkanDeviceDescriptorSet : public RHIDeviceDescriptorSet {
        const VulkanDeviceDescriptorPool& m_pool;
        vk::raii::DescriptorSet m_set{ nullptr };
    public:
        VulkanDeviceDescriptorSet(VulkanDeviceDescriptorPool const& pool, vk::raii::DescriptorSet&& set);

        void Update(UpdateDesc const& desc) override;

        auto const& GetVkDescriptorSet() const { return m_set; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDeviceDescriptorPool : public RHIDeviceDescriptorPool {
        const VulkanDevice& m_device;
        vk::raii::DescriptorPool m_pool{ nullptr };
        RHIObjectStorage<> m_storage;
    public:
        VulkanDeviceDescriptorPool(const VulkanDevice& device, PoolDesc const& desc);
        RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet> CreateDescriptorSet(
            RHIDeviceObjectHandle<RHIDeviceDescriptorSetLayout> layout) override;
        RHIDeviceDescriptorSet* GetDescriptorSet(Handle handle) const override;

        void DestroyDescriptorSet(Handle handle) override;
        auto const& GetDevice() const { return m_device; }
        auto const& GetVkDescriptorPool() const { return m_pool; }

        void DebugSetObjectName(const char* name) override;
    };
}
