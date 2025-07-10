#include <Platform/RHI/Vulkan/VulkanApplication.hpp>
#include <Platform/RHI/Vulkan/VulkanDevice.hpp>

using namespace Foundation::Platform;
const char* kVulkanInstanceExtensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
};

const char* kVulkanInstanceLayers[] = {
    "VK_LAYER_KHRONOS_validation"
};

VulkanApplication::VulkanApplication(const char* appName, const char* engineName, const uint32_t apiVersion)
    : m_vulkanApiVersion(apiVersion), m_name(appName) {
    auto vkAppInfo = vk::ApplicationInfo(
        appName, 0, engineName, 0, apiVersion
    );
    uint32_t count;
    const char** extensions = glfwGetRequiredInstanceExtensions(&count);
    if (!extensions) {
        throw std::runtime_error("Failed to get required Vulkan instance extensions");
    }
    std::vector<const char*> instanceExtensions(extensions, extensions + count);
    // Add our own extensions
    instanceExtensions.insert(
        instanceExtensions.end(),
        kVulkanInstanceExtensions, kVulkanInstanceExtensions + sizeof(kVulkanInstanceExtensions) / sizeof(kVulkanInstanceExtensions[0])
    );
    m_instance = vk::raii::Instance(m_context, vk::InstanceCreateInfo(
        {},
        &vkAppInfo,
        kVulkanInstanceLayers,
        instanceExtensions
    ));
    for (const auto& physicalDevice : vk::raii::PhysicalDevices(m_instance)) {
        m_devices.emplace_back(std::make_shared<VulkanDevice>(*this, physicalDevice));
    }
}

std::vector<std::weak_ptr<RHIDevice>> VulkanApplication::EnumerateDevices() const {
    return std::vector<std::weak_ptr<RHIDevice>>(m_devices.begin(), m_devices.end());            
}

