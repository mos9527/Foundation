using namespace Foundation;
using namespace Core;
using namespace RHI;
const char* kVulkanDesiredInstanceExtensions[] = {
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,  // Shader printfs
    VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME // For non-sRGB color spaces
};
static VKAPI_ATTR VkBool32 VKAPI_CALL
VkDebugLayerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                     const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void*)
{
    constexpr LogLevel kLevels[] = {LogDebug, LogInfo, LogWarn, LogError};
    LOG(VkDebugLayer, kLevels[std::countr_zero(static_cast<uint32_t>(severity)) >> 2 & 3], "{}", pCallbackData->pMessage);
    return vk::False;
}
VulkanApplication::VulkanApplication(Allocator* allocator, bool headless, const char* appName, const char* engineName,
                                     const uint32_t apiVersion) :
    mAllocator(allocator), mVkAllocationCallbacks(nullptr /* NOTE: Many drivers do not like this roundtrip to our own code. Disabled for sanity. */),
    mPhysicalDevices(allocator), mDevices(allocator), mStorage(allocator), mName(Format("{} powered by {}", appName, engineName)), mVulkanApiVersion(apiVersion), mHeadless(headless)
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
    auto availableExtensions = VkEnumerate<VkExtensionProperties>(
        [&](uint32_t* count, VkExtensionProperties* data) {
            return mContext.getDispatcher()->vkEnumerateInstanceExtensionProperties(nullptr, count, data);
        }, mAllocator);
    auto isExtensionAvailable = [&](const char* extName) -> bool {
        for (auto const& ext : availableExtensions) {
            if (StringView(ext.extensionName) == extName)
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
    auto availableLayers = VkEnumerate<VkLayerProperties>(
        [&](uint32_t* count, VkLayerProperties* data) {
            return mContext.getDispatcher()->vkEnumerateInstanceLayerProperties(count, data);
        }, mAllocator);
    auto isLayerAvailable = [&](const char* layerName) -> bool {
        for (auto const& layer : availableLayers) {
            if (StringView(layer.layerName) == layerName)
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
    mInstance = VkExpect(mContext.createInstance(instanceInfo, GetVkAllocationCallbacks()));    
    mDevices.clear();
    mPhysicalDevices.clear();
    auto physicalDevices = VkEnumerate<VkPhysicalDevice>(
        [&](uint32_t* count, VkPhysicalDevice* data) {
            return mInstance.getDispatcher()->vkEnumeratePhysicalDevices(static_cast<VkInstance>(*mInstance), count, data);
        }, mAllocator);
    for (uint32_t id = 0; id < physicalDevices.size(); ++id)
    {
        vk::raii::PhysicalDevice device(mInstance, physicalDevices[id]);
        auto props = device.getProperties();
        mDevices.emplace_back(RHIDevice::DeviceDesc{.id = id, .name = props.deviceName.data() });
        mPhysicalDevices.emplace_back(std::move(device));
    }
    if (isExtensionEnabled(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        VkDebugUtilsMessengerCreateInfoEXT debugInfo{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .pNext = nullptr,
            .flags = 0,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = &VkDebugLayerCallback,
            .pUserData = nullptr
        };
        mDebugHandler = VkExpect(mInstance.createDebugUtilsMessengerEXT(
            reinterpret_cast<const vk::DebugUtilsMessengerCreateInfoEXT&>(debugInfo),
            GetVkAllocationCallbacks()));
    }
}

VulkanApplication::~VulkanApplication()
{
    mStorage.Destroy();
    mDebugHandler.clear();
    mInstance.clear();
}

Span<const RHIDevice::DeviceDesc> VulkanApplication::EnumerateDevices() const
{
    return {mDevices.begin(), mDevices.end()};
}

RHIApplicationScopedHandle<RHIDevice> VulkanApplication::CreateDevice(RHIDevice::DeviceDesc const& desc)
{
    CHECK_MSG(desc.id < mPhysicalDevices.size(), "Invalid device id. Total {} devices, requested {}", mPhysicalDevices.size(), desc.id);
    auto& phys_device = mPhysicalDevices[desc.id];
    LOG(VulkanApplication, LogInfo, "Using device {} (#{})", mDevices[desc.id].name, desc.id);
    Handle handle = mStorage.CreateObject<VulkanDevice>(*this, phys_device);
    return {this, handle};
}
RHIDevice* VulkanApplication::GetDevice(Handle handle) const { return mStorage.GetObjectPtr<RHIDevice>(handle); }
void VulkanApplication::DestroyDevice(Handle handle) { mStorage.DestroyObject(handle); }

String VulkanApplication::ResolveRelativePathBase(StringView path) const
{
    const char* basePath = SDL_GetBasePath();
    if (!basePath)
        return String(path);
    if (path.empty())
        return String(basePath);
    return Format("{}/{}", basePath, path);
}

String VulkanApplication::ResolveRelativePathData(StringView path) const
{
    const char* basePath = SDL_GetPrefPath("", mName.c_str());
    if (!basePath)
        return String(path);
    if (path.empty())
        return String(basePath);
    return Format("{}/{}", basePath, path);
}

Optional<RHIFileInfo> VulkanApplication::QueryFileInfo(StringView path) const
{
    SDL_PathInfo info{};
    if (SDL_GetPathInfo(path.data(), &info))
    {        
        return RHIFileInfo{
            .size = info.size,
            .ctime = SDL_NS_TO_SECONDS(info.create_time),
            .mtime = SDL_NS_TO_SECONDS(info.modify_time),
            .atime = SDL_NS_TO_SECONDS(info.access_time),
            .isDirectory = info.type == SDL_PATHTYPE_DIRECTORY,
        };
    }
    return {};
}
struct SDLIteratorCallbackData
{
    RHIDirectoryIteratorCallback fnActual;
    void* userData;
};
static SDL_EnumerationResult SDLDirectoryIteratorCallback(void* userData, const char* directory, const char* file)
{
    auto* data = static_cast<SDLIteratorCallbackData*>(userData);
    return data->fnActual(data->userData, directory, file) ? SDL_ENUM_CONTINUE : SDL_ENUM_SUCCESS;
}
bool VulkanApplication::IterateDirectory(StringView path, RHIDirectoryIteratorCallback cb, void* userData) const {
    SDLIteratorCallbackData data{.fnActual = cb, .userData = userData};
    return SDL_EnumerateDirectory(path.data(), SDLDirectoryIteratorCallback, &data);
}
bool VulkanApplication::CreateDirectory(StringView path) const { return SDL_CreateDirectory(path.data()); }
bool VulkanApplication::RemoveDirectory(StringView path) const { return SDL_RemovePath(path.data()); }
bool VulkanApplication::RemoveFile(StringView path) const { return SDL_RemovePath(path.data()); }
