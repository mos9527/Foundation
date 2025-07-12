#pragma once
#include <Core/Core.hpp>
#include <Core/Allocator/Allocator.hpp>

#include <Platform/RHI/Application.hpp>
#include <Platform/RHI/Details/Resource.hpp>
#include <Platform/RHI/Vulkan/Device.hpp>

#include <Platform/RHI/Vulkan/Common.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            class VulkanDevice;
            class VulkanApplication : public RHIApplication {                                                
                vk::raii::PhysicalDevices m_physicalDevices{ nullptr };

                RHIObjectStorage m_storage;
            public:
                const std::string m_name;
                Core::Allocator* m_allocator;

                const vk::raii::Context m_context;
                const uint32_t m_vulkanApiVersion;
                vk::raii::Instance m_instance{ nullptr };

                VulkanApplication(const char* appName, const char* engineName, const uint32_t apiVersion, Core::Allocator* allocator);
                ~VulkanApplication() = default;

                std::vector<RHIDevice::DeviceDesc> EnumerateDevices() const override;

                RHIApplicationScopedObjectHandle<RHIDevice> CreateDevice(const RHIDevice::DeviceDesc& desc) override;
                RHIDevice* GetDevice(Handle handle) const override;
                void DestroyDevice(Handle handle) override;

                inline bool IsValid() const override { return m_instance != nullptr; }
                inline const char* GetName() const override { return m_name.c_str(); }
            };

        }
    }
}
