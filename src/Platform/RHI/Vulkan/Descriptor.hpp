#pragma once
#include <Platform/RHI/Descriptor.hpp>
#include <Platform/RHI/Vulkan/Common.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            class VulkanDevice;
            class VulkanDeviceDescriptorPool;
            class VulkanDeviceDescriptorSetLayout;
            class VulkanDeviceDescriptorSet : public RHIDeviceDescriptorSet {
                const VulkanDeviceDescriptorPool& m_pool;
                vk::raii::DescriptorSet m_set{ nullptr };
            public:
                VulkanDeviceDescriptorSet(VulkanDeviceDescriptorPool const& pool, vk::raii::DescriptorSet&& set);

                void Update(UpdateDesc const& desc) override;

                inline auto const& GetVkDescriptorSet() const { return m_set; }
                inline bool IsValid() const override { return m_set != nullptr; }
                inline const char* GetName() const override { return "VulkanDeviceDescriptorSet"; }
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
                inline auto const& GetDevice() const { return m_device; }
                inline auto const& GetVkDescriptorPool() const { return m_pool; }
                inline bool IsValid() const override { return m_pool != nullptr; }
                inline const char* GetName() const override { return "VulkanDeviceDescriptorPool"; }
            };
        }
    }
}
