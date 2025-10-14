#pragma once
#include <RHICore/Application.hpp>

#include "Common.hpp"
#include "Device.hpp"
namespace Foundation::RHI {
    extern "C" {
        // Custom allocation callbacks for Vulkan on CPU
        void* vkCustomCpuAllocation(Core::Allocator* alloc, size_t size, size_t alignment, vk::SystemAllocationScope allocationScope);
        void* vkCustomCpuReallocation(Core::Allocator* alloc, void* pOriginal, size_t size, size_t alignment, vk::SystemAllocationScope allocationScope);
        void vkCustomCpuFree(Core::Allocator* alloc, void* pMemory);
        inline vk::AllocationCallbacks CreateVulkanCpuAllocationCallbacks(Core::Allocator* alloc) {
            return vk::AllocationCallbacks{
                .pUserData = alloc,
                .pfnAllocation = reinterpret_cast<vk::PFN_AllocationFunction>(vkCustomCpuAllocation),
                .pfnReallocation = reinterpret_cast<vk::PFN_ReallocationFunction>(vkCustomCpuReallocation),
                .pfnFree = reinterpret_cast<vk::PFN_FreeFunction>(vkCustomCpuFree),
                .pfnInternalAllocation = nullptr,
                .pfnInternalFree = nullptr
            };
        }
    }
    class VulkanDevice;
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

        VulkanApplication(Allocator* allocator, const char* appName = "Vulkan RHI", const char* engineName = "Foundation", const uint32_t apiVersion = VK_API_VERSION_1_3);
        Span<const RHIDevice::DeviceDesc> EnumerateDevices() const override;

        RHIApplicationScopedObjectHandle<RHIDevice> CreateDevice(RHIDevice::DeviceDesc const& desc, Native::NativeWindow* window = nullptr) override;
        RHIDevice* GetDevice(Handle handle) const override;
        void DestroyDevice(Handle handle) override;

        Allocator* GetAllocator() const { return mAllocator; }
        vk::AllocationCallbacks const& GetVkAllocatorCallbacks() const { return mVkAllocatorCpuCallbacks; }
        auto const& GetVkInstance() const { return mInstance; }
    };
}
