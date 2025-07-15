#include <Platform/RHI/Vulkan/Application.hpp>
#include <Platform/RHI/Vulkan/Device.hpp>
#include <Core/Logging.hpp>
using namespace Foundation::Platform::RHI;
const char* kVulkanInstanceExtensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME
};

const char* kVulkanInstanceLayers[] = {
    "VK_LAYER_KHRONOS_validation"
};

static VKAPI_ATTR vk::Bool32 VKAPI_CALL VkDebugLayerCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
    vk::DebugUtilsMessageTypeFlagsEXT type,
    const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData, void*
) {
    LOG_RUNTIME(VulkanDebugLayer, err,
        "{}",
        pCallbackData->pMessage
    );
    return vk::False;
}
VulkanApplication::~VulkanApplication() {
    // Destroy all devices first
    m_storage.Clear();
}
VulkanApplication::VulkanApplication(const char* appName, const char* engineName, const uint32_t apiVersion, Core::Allocator* allocator)
    : m_vulkanApiVersion(apiVersion), m_name(appName), m_allocator(allocator), m_devices(allocator), 
    m_storage(allocator), m_vkAllocatorCpuCallbacks(CreateVulkanCpuAllocationCallbacks(allocator))
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
    }, m_vkAllocatorCpuCallbacks);
    m_physicalDevices = vk::raii::PhysicalDevices(m_instance);
    m_devices.clear();    
    for (uint32_t id = 0; id < m_physicalDevices.size(); ++id) {
        auto const& device = m_physicalDevices[id];
        auto props = device.getProperties();
        m_devices.emplace_back(RHIDevice::DeviceDesc{
            .id = id,
            .name = props.deviceName
        });
    }
    // Debug layer callbacks
    m_debug_handler = m_instance.createDebugUtilsMessengerEXT({
        .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
        .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation,
        .pfnUserCallback = &VkDebugLayerCallback
    });
}

std::span<const RHIDevice::DeviceDesc> VulkanApplication::EnumerateDevices() const {
    return { m_devices.begin(), m_devices.end() };
}

RHIApplicationScopedObjectHandle<RHIDevice> VulkanApplication::CreateDevice(const RHIDevice::DeviceDesc& desc, Window* window) {
    auto& phys_device = m_physicalDevices[desc.id];
    Handle handle = m_storage.CreateObject<VulkanDevice>(window, *this, phys_device);
    return { this, handle };
}
RHIDevice* VulkanApplication::GetDevice(Handle handle) const {
    return m_storage.GetObjectPtr<RHIDevice>(handle);
}
void VulkanApplication::DestroyDevice(Handle handle) {
    m_storage.DestroyObject(handle);
}
// Vulkan Custom Allocation Callbacks
namespace Foundation {
    namespace Platform {
        namespace RHI {
            extern "C" {
                static void* vkCustomCpuAllocation(
                    Foundation::Core::Allocator* alloc, size_t size, size_t alignment,
                    vk::SystemAllocationScope allocationScope)
                {
                    return alloc->Allocate(size, alignment);
                }
                static void* vkCustomCpuReallocation(
                    Foundation::Core::Allocator* alloc, void* pOriginal, size_t size, size_t alignment,
                    vk::SystemAllocationScope allocationScope)
                {
                    return alloc->Reallocate(pOriginal, size, alignment);
                }
                static void vkCustomCpuFree(Foundation::Core::Allocator* alloc, void* pMemory)
                {
                    alloc->Deallocate(pMemory);
                }
            }
        }
    }
}
