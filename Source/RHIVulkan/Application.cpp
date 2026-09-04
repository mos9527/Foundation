#include <SDL3/SDL.h>
#if defined(__ANDROID__)
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>
#include <SDL3/SDL_system.h>
#endif

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

static String JoinPath(StringView a, StringView b)
{
    const char* seps[] = {"/", "\\"};
    for (auto sep : seps)
    {
        if (!a.empty() && a.ends_with(sep))
            a.remove_suffix(1);
    }
    if (a.empty())
        return b.data();
    if (b.empty())
        return a.data();
    return Format("{}/{}", a, b);
}

#if defined(__ANDROID__)
static constexpr const char* kAndroidAssetDirs[] = {
    "Data",
    "Data/Shaders",
    "Data/Shaders/Editor",
    "Data/Assets",
    "Data/Assets/Matcaps",
    "Data/Assets/ViewLUTs",
    "Data/Icons",
};

static AAssetManager* GetAndroidAssetManager()
{
    static AAssetManager* mgr = nullptr;
    if (mgr)
        return mgr;
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity)
        return nullptr;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getAssets = env->GetMethodID(activityClass, "getAssets", "()Landroid/content/res/AssetManager;");
    jobject assetManager = getAssets ? env->CallObjectMethod(activity, getAssets) : nullptr;
    if (assetManager)
        mgr = AAssetManager_fromJava(env, assetManager);
    if (assetManager)
        env->DeleteLocalRef(assetManager);
    if (activityClass)
        env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return mgr;
}

static bool ExtractAndroidAssetFile(AAssetManager* mgr, StringView assetPath, StringView destPath)
{
    String assetStr(assetPath);
    AAsset* asset = AAssetManager_open(mgr, assetStr.c_str(), AASSET_MODE_BUFFER);
    if (!asset)
        return false;
    off_t const size = AAsset_getLength(asset);
    void const* buf = AAsset_getBuffer(asset);
    String destStr(destPath);
    if (auto const sep = destStr.find_last_of("/\\"); sep != String::npos)
        SDL_CreateDirectory(destStr.substr(0, sep).c_str());
    FILE* fp = fopen(destStr.c_str(), "wb");
    bool ok = fp != nullptr;
    if (ok && size > 0)
        ok = buf && fwrite(buf, 1, static_cast<size_t>(size), fp) == static_cast<size_t>(size);
    if (fp)
        fclose(fp);
    AAsset_close(asset);
    return ok;
}

static uint32_t ExtractAndroidAssetDir(AAssetManager* mgr, const char* assetDir, StringView destRoot)
{
    AAssetDir* dir = AAssetManager_openDir(mgr, assetDir);
    if (!dir)
        return 0;
    uint32_t n = 0;
    for (char const* name = AAssetDir_getNextFileName(dir); name; name = AAssetDir_getNextFileName(dir))
    {
        StringView file(name);
        if (file.ends_with(".d"))
            continue;
        String rel = assetDir && assetDir[0] ? Format("{}/{}", assetDir, name) : String(name);
        if (ExtractAndroidAssetFile(mgr, rel, JoinPath(destRoot, rel)))
            ++n;
    }
    AAssetDir_close(dir);
    return n;
}

static bool LooksRelativePath(StringView path)
{
    if (path.empty() || path[0] == '/' || path[0] == '\\')
        return false;
    return !(path.size() >= 2 && path[1] == ':');
}

static StringView AssetPathFromResolved(StringView resolved, StringView basePath)
{
    if (resolved.size() <= basePath.size())
        return {};
    if (resolved.substr(0, basePath.size()) != basePath)
        return {};
    size_t off = basePath.size();
    if (off < resolved.size() && (resolved[off] == '/' || resolved[off] == '\\'))
        ++off;
    return resolved.substr(off);
}

static bool ExtractAndroidAssetIfMissing(AAssetManager* mgr, StringView destRoot, StringView resolvedOrRelative)
{
    String dest(resolvedOrRelative);
    StringView rel = AssetPathFromResolved(resolvedOrRelative, destRoot);
    if (rel.empty())
    {
        if (!LooksRelativePath(resolvedOrRelative))
            return false;
        rel = resolvedOrRelative;
        dest = JoinPath(destRoot, rel);
    }
    SDL_PathInfo info{};
    if (SDL_GetPathInfo(dest.c_str(), &info) && info.type == SDL_PATHTYPE_FILE)
        return true;
    return ExtractAndroidAssetFile(mgr, rel, dest);
}
#endif

VulkanApplication::VulkanApplication(Allocator* allocator, bool headless, const char* appName, const char* engineName,
                                     const uint32_t apiVersion) :
    mAllocator(allocator), mVkAllocationCallbacks(nullptr /* NOTE: Many drivers do not like this roundtrip to our own code. Disabled for sanity. */),
    mPhysicalDevices(allocator), mDevices(allocator), mStorage(allocator), mName(Format("{} powered by {}", appName, engineName)), mVulkanApiVersion(apiVersion), mHeadless(headless)
{
#if defined(__ANDROID__)
    if (char const* internal = SDL_GetAndroidInternalStoragePath())
        mBasePath = internal;
    if (AAssetManager* mgr = GetAndroidAssetManager(); mgr && !mBasePath.empty())
    {
        uint32_t extracted = 0;
        for (char const* dir : kAndroidAssetDirs)
            extracted += ExtractAndroidAssetDir(mgr, dir, mBasePath);
        LOG(VulkanApplication, LogInfo, "Extracted {} APK assets to {}", extracted, mBasePath);
    }
    else
        LOG(VulkanApplication, LogError, "Failed to open Android AssetManager; APK assets will not be extracted.");
#else
    if (char const* basePath = SDL_GetBasePath())
        mBasePath = basePath;
#endif
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
        mDevices.emplace_back(RHIDevice::DeviceDesc{
            .id = id,
            .name = props.deviceName.data(),
            .type = rhiDeviceTypeFromVkPhysicalDeviceType(props.deviceType),
        });
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
    if (mBasePath.empty())
        return String(path);
    if (path.empty())
        return mBasePath;
    String resolved = JoinPath(mBasePath, path);
#if defined(__ANDROID__)
    if (AAssetManager* mgr = GetAndroidAssetManager())
        ExtractAndroidAssetIfMissing(mgr, mBasePath, resolved);
#endif
    return resolved;
}

String VulkanApplication::ResolveRelativePathData(StringView path) const
{
    const char* basePath = SDL_GetPrefPath("", mName.c_str());
    if (!basePath)
        return String(path);
    if (path.empty())
        return String(basePath);
    return JoinPath(basePath, path);
}

Optional<RHIFileInfo> VulkanApplication::QueryFileInfo(StringView path) const
{
    SDL_PathInfo info{};
#if defined(__ANDROID__)
    if (AAssetManager* mgr = GetAndroidAssetManager())
        ExtractAndroidAssetIfMissing(mgr, mBasePath, path);
#endif
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
