#pragma once
#include <RHICore/Application.hpp>

#include "Common.hpp"
#include "Device.hpp"
namespace Foundation::RHI {
    class VulkanDevice;
    class VulkanApplication : public RHIApplication {
        Allocator* mAllocator;
        VulkanAllocationCallbacks mVkAllocationCallbacks;
        vk::raii::Instance mInstance{ nullptr };
        Vector<vk::raii::PhysicalDevice> mPhysicalDevices;

        Vector<RHIDevice::DeviceDesc> mDevices;
        vk::raii::DebugUtilsMessengerEXT mDebugHandler{ nullptr };

        RHIObjectPool<> mStorage;
    public:
        const String mName;

        const vk::raii::Context mContext;
        const uint32_t mVulkanApiVersion;

        VulkanApplication(Allocator* allocator, bool headless = false, const char* appName = "Vulkan RHI", const char* engineName = "Foundation", uint32_t apiVersion = VK_API_VERSION_1_3);
        ~VulkanApplication() override;
        Span<const RHIDevice::DeviceDesc> EnumerateDevices() const override;

        RHIApplicationScopedHandle<RHIDevice> CreateDevice(RHIDevice::DeviceDesc const& desc) override;
        RHIDevice* GetDevice(Handle handle) const override;
        void DestroyDevice(Handle handle) override;
        
        [[nodiscard]] virtual String ResolveRelativePathBase(StringView path = "") const override;
        [[nodiscard]] virtual String ResolveRelativePathData(StringView path = "") const override;
        [[nodiscard]] virtual Optional<RHIFileInfo> QueryFileInfo(StringView path) const override;
        virtual bool IterateDirectory(StringView path, RHIDirectoryIteratorCallback cb, void* userData) const override;
        virtual bool CreateDirectory(StringView path) const override;
        virtual bool RemoveDirectory(StringView path) const override;
        virtual bool RemoveFile(StringView path) const override;

        Allocator* GetAllocator() const { return mAllocator; }
        auto const& GetVkInstance() const { return mInstance; }
        [[nodiscard]] vk::AllocationCallbacks const* GetVkAllocationCallbacks() const { return mVkAllocationCallbacks.Get(); }
        [[nodiscard]] VkAllocationCallbacks const* GetVkAllocationCallbacksNative() const { return mVkAllocationCallbacks.GetNative(); }
    private:
        bool mHeadless{false};
    };
}
