using namespace Foundation;
using namespace Core;
using namespace RHI;
const char* kVulkanDesiredInstanceExtensions[] = {
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,  // Shader printfs
    VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME // For non-sRGB color spaces
};
static VKAPI_ATTR vk::Bool32 VKAPI_CALL
VkDebugLayerCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT severity, vk::DebugUtilsMessageTypeFlagsEXT,
                     const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData, void*)
{
    constexpr LogLevel kLevels[] = {LogDebug, LogInfo, LogWarn, LogError};
    LOG(VkDebugLayer, kLevels[std::countr_zero(static_cast<uint32_t>(severity)) >> 2 & 3], "{}", pCallbackData->pMessage);
    return vk::False;
}
VulkanApplication::VulkanApplication(Allocator* allocator, bool headless, const char* appName, const char* engineName,
                                     const uint32_t apiVersion) :
    mAllocator(allocator), mVkAllocationCallbacks(nullptr /* NOTE: Many drivers do not like this roundtrip to our own code. Disabled for sanity. */),
    mDevices(allocator), mStorage(allocator), mName(appName), mVulkanApiVersion(apiVersion), mHeadless(headless)
{
    auto vkAppInfo = vk::ApplicationInfo{
        .pApplicationName = appName,
        .pEngineName = engineName,
        .apiVersion = apiVersion,
    };
    // SDL-provided WSI instance extensions are only required when presenting to a window.
    // Headless instances (e.g. Lavapipe without WSI) skip them entirely.
    uint32_t sdlExtensionCount = 0;
    char const* const* sdlExtensions = nullptr;
    if (!mHeadless)
    {
        if (!SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount))
            sdlExtensionCount = 0;
        sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
    }
    Vector<const char*> desiredLayers(mAllocator);
#if FOUNDATION_RHIVULKAN_VVL
    desiredLayers.push_back("VK_LAYER_KHRONOS_validation");
#endif
    Ranges::sort(desiredLayers);
    desiredLayers.erase(Ranges::unique(desiredLayers).begin(), desiredLayers.end());

    // --- Extension Querying ---
    auto availableExtensions = mContext.enumerateInstanceExtensionProperties();
    auto isExtensionAvailable = [&](const char* extName) -> bool {
        for (auto const& ext : availableExtensions) {
            if (StringView(ext.extensionName.data()) == extName)
                return true;
        }
        return false;
    };

    Vector<const char*> enabledExtensions(mAllocator);
    bool allRequiredExtensionsAvailable = true;
    for (uint32_t i = 0; i < sdlExtensionCount; ++i) {
        if (isExtensionAvailable(sdlExtensions[i])) {
            enabledExtensions.push_back(sdlExtensions[i]);
        } else {
            LOG(VulkanApplication, LogError, "Required instance extension '{}' is not available.", sdlExtensions[i]);
            allRequiredExtensionsAvailable = false;
        }
    }
    for (auto* desired : kVulkanDesiredInstanceExtensions) {
        if (isExtensionAvailable(desired)) {
            enabledExtensions.push_back(desired);
        } else {
            LOG(VulkanApplication, LogWarn, "Instance extension '{}' is not available and will not be enabled.", desired);
        }
    }
    Ranges::sort(enabledExtensions);
    enabledExtensions.erase(Ranges::unique(enabledExtensions).begin(), enabledExtensions.end());
    CHECK_MSG(allRequiredExtensionsAvailable,
              "One or more required Vulkan instance extensions are not available (see log above).");

    auto isExtensionEnabled = [&](const char* extName) -> bool {
        for (auto* ext : enabledExtensions) {
            if (StringView(ext) == extName)
                return true;
        }
        return false;
    };

    // --- Layer Querying ---
    auto availableLayers = mContext.enumerateInstanceLayerProperties();
    auto isLayerAvailable = [&](const char* layerName) -> bool {
        for (auto const& layer : availableLayers) {
            if (StringView(layer.layerName.data()) == layerName)
                return true;
        }
        return false;
    };

    Vector<const char*> enabledLayers(mAllocator);
    for (auto* desired : desiredLayers) {
        if (isLayerAvailable(desired)) {
            enabledLayers.push_back(desired);
        } else {
            LOG(VulkanApplication, LogWarn, "Instance layer '{}' is not available and will not be enabled.", desired);
        }
    }

    vk::InstanceCreateInfo instanceInfo{
        .pApplicationInfo = &vkAppInfo,
        .enabledLayerCount = static_cast<uint32_t>(enabledLayers.size()),
        .ppEnabledLayerNames = enabledLayers.data(),
        .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
        .ppEnabledExtensionNames = enabledExtensions.data(),
    };
#if FOUNDATION_RHIVULKAN_VVL
    // Enable shader printf
    VkValidationFeatureEnableEXT validation_feature_enables = VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT;
    VkValidationFeaturesEXT validation_features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation_features.enabledValidationFeatureCount = 1;
    validation_features.pEnabledValidationFeatures = &validation_feature_enables;
    instanceInfo.setPNext(&validation_features);
#endif
    mInstance = vk::raii::Instance(mContext, instanceInfo, GetVkAllocationCallbacks());
    mPhysicalDevices = vk::raii::PhysicalDevices(mInstance);
    mDevices.clear();
    for (uint32_t id = 0; id < mPhysicalDevices.size(); ++id)
    {
        auto const& device = mPhysicalDevices[id];
        auto props = device.getProperties();
        mDevices.emplace_back(RHIDevice::DeviceDesc{.id = id, .name = props.deviceName});
    }
    if (isExtensionEnabled(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        mDebugHandler = mInstance.createDebugUtilsMessengerEXT(
            {.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
                 vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                 vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
             .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                 vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation,
             .pfnUserCallback = &VkDebugLayerCallback},
            GetVkAllocationCallbacks());
    }
}

VulkanApplication::~VulkanApplication()
{
    mStorage.Destroy();
    mDebugHandler = nullptr;
    mInstance = nullptr;
}

Span<const RHIDevice::DeviceDesc> VulkanApplication::EnumerateDevices() const
{
    return {mDevices.begin(), mDevices.end()};
}

RHIApplicationScopedHandle<RHIDevice> VulkanApplication::CreateDevice(RHIDevice::DeviceDesc const& desc,
                                                                            SDL_Window* window)
{
    CHECK_MSG(desc.id < mPhysicalDevices.size(), "Invalid device id. Total {} devices, requested {}", mPhysicalDevices.size(), desc.id);
    auto& phys_device = mPhysicalDevices[desc.id];
    LOG(VulkanApplication, LogInfo, "Using device {} (#{})", mDevices[desc.id].name, desc.id);
    Handle handle = mStorage.CreateObject<VulkanDevice>(*this, phys_device, window);
    return {this, handle};
}
RHIDevice* VulkanApplication::GetDevice(Handle handle) const { return mStorage.GetObjectPtr<RHIDevice>(handle); }
void VulkanApplication::DestroyDevice(Handle handle) { mStorage.DestroyObject(handle); }

