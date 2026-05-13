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
        vk::raii::PhysicalDevices mPhysicalDevices{ nullptr };

        Vector<RHIDevice::DeviceDesc> mDevices;
        vk::raii::DebugUtilsMessengerEXT mDebugHandler{ nullptr };

        RHIObjectPool<> mStorage;
    public:
        const String mName;

        const vk::raii::Context mContext;
        const uint32_t mVulkanApiVersion;

        VulkanApplication(Allocator* allocator, const char* appName = "Vulkan RHI", const char* engineName = "Foundation", uint32_t apiVersion = VK_API_VERSION_1_3);
        ~VulkanApplication() override;
        Span<const RHIDevice::DeviceDesc> EnumerateDevices() const override;

        RHIApplicationScopedHandle<RHIDevice> CreateDevice(RHIDevice::DeviceDesc const& desc, SDL_Window* window) override;
        RHIApplicationScopedHandle<RHIDevice> CreateDevice(RHIDevice::DeviceDesc const& desc)
        {
            return CreateDevice(desc, nullptr);
        }
        RHIDevice* GetDevice(Handle handle) const override;
        void DestroyDevice(Handle handle) override;

        Allocator* GetAllocator() const { return mAllocator; }
        auto const& GetVkInstance() const { return mInstance; }
        [[nodiscard]] vk::AllocationCallbacks const* GetVkAllocationCallbacks() const { return mVkAllocationCallbacks.Get(); }
        [[nodiscard]] VkAllocationCallbacks const* GetVkAllocationCallbacksNative() const { return mVkAllocationCallbacks.GetNative(); }
    };
}
