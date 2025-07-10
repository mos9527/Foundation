#pragma once
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vma/vk_mem_alloc.h>

#include <Core/Core.hpp>
#include <Platform/RHI/RHI.hpp>
namespace Foundation {
    namespace Platform {
        inline vk::Format GetVulkanFormatFromRHIFormat(RHIResourceFormat format) {
            using enum RHIResourceFormat;
            switch (format) {
                case R8G8B8A8_UNORM: return vk::Format::eR8G8B8A8Unorm;               
                default:
                    Core::BugCheck("illegal format specified");
                    return vk::Format::eUndefined;
            }
        }

        class VulkanDevice;
        class VulkanApplication : public RHIApplication {
            // Owning vector of VulkanDevice instances            
            std::vector<std::shared_ptr<VulkanDevice>> m_devices;
            std::string m_name;
        public:
            const vk::raii::Context m_context;
            const uint32_t m_vulkanApiVersion;
            vk::raii::Instance m_instance{ nullptr };

            VulkanApplication(const char* appName, const char* engineName, const uint32_t apiVersion);
            std::vector<std::weak_ptr<RHIDevice>> EnumerateDevices() const;

            inline const char* GetName() const override {
                return m_name.c_str();
            }
        };

    }
}
