#pragma once
#include <RHICore/Descriptor.hpp>
#include "Common.hpp"

namespace Foundation::RHI {
    class VulkanDevice;
    class VulkanDeviceDescriptorPool;
    class VulkanDeviceDescriptorSetLayout;
    class VulkanDeviceDescriptorSet : public RHIDeviceDescriptorSet {
        const VulkanDeviceDescriptorPool& mPool;
        vk::raii::DescriptorSet mSet{ nullptr };
    public:
        VulkanDeviceDescriptorSet(VulkanDeviceDescriptorPool const& pool, vk::raii::DescriptorSet&& set);

        void Update(UpdateDesc const& desc) override;

        [[nodiscard]] auto const& GetVkDescriptorSet() const { return mSet; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDeviceDescriptorPool : public RHIDeviceDescriptorPool {
        const VulkanDevice& mDevice;
        vk::raii::DescriptorPool mPool{ nullptr };
        RHIObjectPool<> mStorage;
    public:
        VulkanDeviceDescriptorPool(const VulkanDevice& device, PoolDesc const& desc);
        /**
         * @brief Create a descriptor set from this pool.
         * @param layout The layout the descriptor set should conform to.
         * @param max_variable_count Maximum number of _variable number of_ descriptors. Set to 0 if the layout does not have any variable count bindings.
         */
        RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet> CreateDescriptorSet(
            RHIDeviceObjectHandle<RHIDeviceDescriptorSetLayout> layout, uint32_t max_variable_count) override;
        RHIDeviceDescriptorSet* GetDescriptorSet(Handle handle) const override;

        void DestroyDescriptorSet(Handle handle) override;
        auto const& GetDevice() const { return mDevice; }
        auto const& GetVkDescriptorPool() const { return mPool; }

        void DebugSetObjectName(const char* name) override;
    };
}
