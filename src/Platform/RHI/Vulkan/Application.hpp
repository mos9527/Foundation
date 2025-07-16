#pragma once
#include <Core/Core.hpp>
#include <Core/Allocator/StlContainers.hpp>

#include <Platform/RHI/Application.hpp>
#include <Platform/RHI/Vulkan/Device.hpp>
#include <Platform/RHI/Vulkan/Common.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
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
                vk::AllocationCallbacks m_vkAllocatorCpuCallbacks;
                vk::raii::PhysicalDevices m_physicalDevices{ nullptr };
                vk::raii::Instance m_instance{ nullptr };
                Core::Allocator* m_allocator;
                RHIObjectStorage<> m_storage;

                Core::StlVector<RHIDevice::DeviceDesc> m_devices;
                vk::raii::DebugUtilsMessengerEXT m_debug_handler{ nullptr };
            public:
                const std::string m_name;

                const vk::raii::Context m_context;
                const uint32_t m_vulkanApiVersion;

                VulkanApplication(const char* appName, const char* engineName, const uint32_t apiVersion, Core::Allocator* allocator);
                ~VulkanApplication();

                Core::StlSpan<const RHIDevice::DeviceDesc> EnumerateDevices() const override;

                RHIApplicationScopedObjectHandle<RHIDevice> CreateDevice(const RHIDevice::DeviceDesc& desc, Window* window) override;
                RHIDevice* GetDevice(Handle handle) const override;
                void DestroyDevice(Handle handle) override;

                inline Core::Allocator* GetAllocator() const { return m_allocator; }
                inline vk::AllocationCallbacks const& GetVkAllocatorCallbacks() const { return m_vkAllocatorCpuCallbacks; }
                inline auto const& GetVkInstance() const { return m_instance; }

                inline bool IsValid() const override { return m_instance != nullptr; }
                inline const char* GetName() const override { return m_name.c_str(); }
            };

        }
    }
}
