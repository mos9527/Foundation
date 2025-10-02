#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <Core/Core.hpp>
#include <Bits/Ranges.hpp>
#include "Application.hpp"
#include "Device.hpp"

using namespace Foundation;
using namespace Core;
using namespace RHI;
using namespace Bits;
const char* kVulkanInstanceExtensions[] = {
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME
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
VulkanApplication::VulkanApplication(Allocator* allocator, const char* appName, const char* engineName, const uint32_t apiVersion)
    : mVkAllocatorCpuCallbacks(CreateVulkanCpuAllocationCallbacks(allocator)), mAllocator(allocator), mStorage(allocator), mDevices(allocator),
    mName(appName), mVulkanApiVersion(apiVersion)
{
    auto vkAppInfo = vk::ApplicationInfo{
        .pApplicationName = appName,
        .pEngineName = engineName,
        .apiVersion = apiVersion,
    };
    uint32_t count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&count);
    Vector<const char*> instanceExtensions(mAllocator);
    instanceExtensions.insert(instanceExtensions.end(), extensions, extensions + count);
    // Add our own extensions
    instanceExtensions.insert(
        instanceExtensions.end(),
        kVulkanInstanceExtensions, kVulkanInstanceExtensions + std::size(kVulkanInstanceExtensions)
    );
    Vector<const char*> instanceLayers(mAllocator);
#if FOUNDATION_RHIVULKAN_VVL
    instanceLayers.push_back("VK_LAYER_KHRONOS_validation");
#endif
    Ranges::sort(instanceLayers);
    instanceLayers.erase(Ranges::unique(instanceLayers).begin(), instanceLayers.end());
    Ranges::sort(instanceExtensions);
    instanceExtensions.erase(Ranges::unique(instanceExtensions).begin(), instanceExtensions.end());
    vk::InstanceCreateInfo instanceInfo{
        .pApplicationInfo = &vkAppInfo,
        .enabledLayerCount = static_cast<uint32_t>(instanceLayers.size()),
        .ppEnabledLayerNames = instanceLayers.data(),
        .enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size()),
        .ppEnabledExtensionNames = instanceExtensions.data(),
    };
#if FOUNDATION_RHIVULKAN_VVL
    // Enable shader printf
    VkValidationFeatureEnableEXT validation_feature_enables = VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT;
    VkValidationFeaturesEXT validation_features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation_features.enabledValidationFeatureCount = 1;
    validation_features.pEnabledValidationFeatures = &validation_feature_enables;
    instanceInfo.setPNext(&validation_features);
#endif
    mInstance = vk::raii::Instance(mContext, instanceInfo, mVkAllocatorCpuCallbacks);
    mPhysicalDevices = vk::raii::PhysicalDevices(mInstance);
    mDevices.clear();
    for (uint32_t id = 0; id < mPhysicalDevices.size(); ++id) {
        auto const& device = mPhysicalDevices[id];
        auto props = device.getProperties();
        mDevices.emplace_back(RHIDevice::DeviceDesc{
            .id = id,
            .name = props.deviceName
            });
    }
    // Debug layer callbacks
    mDebugHandler = mInstance.createDebugUtilsMessengerEXT({
        .messageSeverity = 
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose | 
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo | 
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | 
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
        .messageType = 
            vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | 
            vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance | 
            vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation,
        .pfnUserCallback = &VkDebugLayerCallback
    });
}

Span<const RHIDevice::DeviceDesc> VulkanApplication::EnumerateDevices() const {
    return { mDevices.begin(), mDevices.end() };
}

RHIApplicationScopedObjectHandle<RHIDevice> VulkanApplication::CreateDevice(RHIDevice::DeviceDesc const& desc, Native::NativeWindow* window) {
    auto& phys_device = mPhysicalDevices[desc.id];
    Handle handle = mStorage.CreateObject<VulkanDevice>(*this, phys_device, window);
    return { this, handle };
}
RHIDevice* VulkanApplication::GetDevice(Handle handle) const {
    return mStorage.GetObjectPtr<RHIDevice>(handle);
}
void VulkanApplication::DestroyDevice(Handle handle) {
    mStorage.DestroyObject(handle);
}
// Vulkan Custom Allocation Callbacks
namespace Foundation::RHI {
    extern "C" {
        void* vkCustomCpuAllocation(
            Allocator* alloc, size_t size, size_t alignment,
            vk::SystemAllocationScope allocationScope)
        {
            return alloc->Allocate(size, alignment);
        }
        void* vkCustomCpuReallocation(
            Allocator* alloc, void* pOriginal, size_t size, size_t alignment,
            vk::SystemAllocationScope allocationScope)
        {
            return alloc->Reallocate(pOriginal, size, alignment);
        }
        void vkCustomCpuFree(Allocator* alloc, void* pMemory)
        {
            alloc->Deallocate(pMemory);
        }
    }
}
