#pragma once
#include <SDL3/SDL.h>
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

        Allocator* GetAllocator() const { return mAllocator; }
        auto const& GetVkInstance() const { return mInstance; }
        [[nodiscard]] vk::AllocationCallbacks const* GetVkAllocationCallbacks() const { return mVkAllocationCallbacks.Get(); }
        [[nodiscard]] VkAllocationCallbacks const* GetVkAllocationCallbacksNative() const { return mVkAllocationCallbacks.GetNative(); }
        /**
         * @brief Whether this instance was created without Vulkan WSI (window/surface) extensions.
         *
         * Headless instances cannot create a hardware presentation surface or swapchain; they are intended
         * for offscreen rendering (e.g. software rasterizers such as Lavapipe that do not expose WSI).
         */
        [[nodiscard]] bool IsHeadless() const { return mHeadless; }
    private:
        bool mHeadless{false};
    };
}
