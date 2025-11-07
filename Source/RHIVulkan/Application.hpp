#pragma once
#include <SDL3/SDL.h>
#include <RHICore/Application.hpp>

#include "Common.hpp"
#include "Device.hpp"
namespace Foundation::RHI {
    extern "C" {
        // Custom allocation callbacks for Vulkan on CPU
        void* vkCustomCpuAllocation(Allocator* alloc, size_t size, size_t alignment, vk::SystemAllocationScope allocationScope);
        void* vkCustomCpuReallocation(Allocator* alloc, void* pOriginal, size_t size, size_t alignment, vk::SystemAllocationScope allocationScope);
        void vkCustomCpuFree(Allocator* alloc, void* pMemory);
    }
    vk::AllocationCallbacks vkCreateVulkanCpuAllocationCallbacks(Allocator* alloc);
    class VulkanDevice;
    class VulkanWindow : public RHIWindow
    {
        mutable SDL_Window* mWindow = nullptr;
        public:
            VulkanWindow(VulkanApplication const& app, WindowDesc const& desc);
            SDL_Window* GetVkWindow() const { return mWindow; }
    };
    class VulkanApplication : public RHIApplication {
        vk::AllocationCallbacks mVkAllocatorCpuCallbacks;
        vk::raii::PhysicalDevices mPhysicalDevices{ nullptr };
        vk::raii::Instance mInstance{ nullptr };
        Allocator* mAllocator;

        Vector<RHIDevice::DeviceDesc> mDevices;
        vk::raii::DebugUtilsMessengerEXT mDebugHandler{ nullptr };

        RHIObjectPool<> mStorage;
    public:
        const String mName;

        const vk::raii::Context mContext;
        const uint32_t mVulkanApiVersion;

        VulkanApplication(Allocator* allocator, const char* appName = "Vulkan RHI", const char* engineName = "Foundation", uint32_t apiVersion = VK_API_VERSION_1_3);
        Span<const RHIDevice::DeviceDesc> EnumerateDevices() const override;

        RHIApplicationScopedObjectHandle<RHIWindow> CreateWindow(RHIWindow::WindowDesc const& desc) override;
        RHIWindow* GetWindow(Handle handle) const override;
        void DestroyWindow(Handle handle) override;

        RHIApplicationScopedObjectHandle<RHIDevice> CreateDevice(RHIDevice::DeviceDesc const& desc, RHIWindow* window = nullptr) override;
        RHIDevice* GetDevice(Handle handle) const override;
        void DestroyDevice(Handle handle) override;

        Allocator* GetAllocator() const { return mAllocator; }
        vk::AllocationCallbacks const& GetVkAllocatorCallbacks() const { return mVkAllocatorCpuCallbacks; }
        auto const& GetVkInstance() const { return mInstance; }
    };
}
