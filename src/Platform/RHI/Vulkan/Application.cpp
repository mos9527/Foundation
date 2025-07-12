#include <Platform/RHI/Vulkan/Application.hpp>
#include <Platform/RHI/Vulkan/Device.hpp>

using namespace Foundation::Platform::RHI;
const char* kVulkanInstanceExtensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
};

const char* kVulkanInstanceLayers[] = {
    "VK_LAYER_KHRONOS_validation"
};

VulkanApplication::VulkanApplication(const char* appName, const char* engineName, const uint32_t apiVersion, Core::Allocator* allocator)
    : m_vulkanApiVersion(apiVersion), m_name(appName), m_allocator(allocator),
    m_storage(allocator)
{
    auto vkAppInfo = vk::ApplicationInfo{
        .pApplicationName = appName,
        .pEngineName = engineName,
        .apiVersion = apiVersion,
    };
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
    m_instance = vk::raii::Instance(m_context, vk::InstanceCreateInfo{
        .pApplicationInfo = &vkAppInfo,
        .enabledLayerCount = sizeof(kVulkanInstanceLayers) / sizeof(kVulkanInstanceLayers[0]),
        .ppEnabledLayerNames = kVulkanInstanceLayers,
        .enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size()),
        .ppEnabledExtensionNames = instanceExtensions.data(),
    });
    m_physicalDevices = vk::raii::PhysicalDevices(m_instance);
}

std::vector<RHIDevice::DeviceDesc> VulkanApplication::EnumerateDevices() const {
    std::vector<RHIDevice::DeviceDesc> descs;
    descs.reserve(m_physicalDevices.size());
    for (uint32_t id = 0; id < m_physicalDevices.size(); ++id) {
        auto const& device = m_physicalDevices[id];
        auto props = device.getProperties();
        descs.emplace_back(RHIDevice::DeviceDesc{
            .id = id,
            .name = props.deviceName
        });
    }
    return descs;
}

RHIApplicationScopedObjectHandle<RHIDevice> VulkanApplication::CreateDevice(const RHIDevice::DeviceDesc& desc) {
    auto& phys_device = m_physicalDevices[desc.id];
    Handle handle = m_storage.CreateObject<VulkanDevice>(desc.window, *this, phys_device, m_allocator);
    return { this, handle };
}
RHIDevice* VulkanApplication::GetDevice(Handle handle) const {
    return m_storage.GetObjectPtr<RHIDevice>(handle);
}
void VulkanApplication::DestroyDevice(Handle handle) {
    m_storage.DestoryObject(handle);
}
