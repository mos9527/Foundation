#define VMA_IMPLEMENTATION
#include "Device.hpp"

#include <queue>
#include <string_view>
#include <type_traits>
#include <vk_mem_alloc.h>

using namespace Foundation::Core;
using namespace Foundation::RHI;
const char* kVulkanDesiredDeviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                                VK_EXT_MESH_SHADER_EXTENSION_NAME,
                                                VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
                                                VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                                VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                                                VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                                VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
                                                VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME};

const char* kVulkanDeviceTypes[] = {"Other", "Integrated GPU", "Discrete GPU", "Virtual GPU", "CPU"};

namespace
{
    constexpr uint64_t kFnvPrime = 1099511628211ull;

    void HashBytes(uint64_t& hash, const void* data, size_t size)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ bytes[i]) * kFnvPrime;
    }

    template <typename T>
    void HashValue(uint64_t& hash, T const& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        HashBytes(hash, &value, sizeof(T));
    }

    RHIPipelineStateCacheKey MakePipelineCacheKey(vk::PhysicalDeviceProperties const& properties)
    {
        uint64_t high = 14695981039346656037ull;
        uint64_t low = 1099511628211ull;
        constexpr auto backend = RHIPipelineStateCacheBackend::Vulkan;
        constexpr auto cacheVersion = RHIPipelineStateCache::kSerializedDataVersion;
        HashValue(high, backend);
        HashValue(high, properties.vendorID);
        HashValue(high, properties.deviceID);
        HashBytes(high, properties.pipelineCacheUUID, VK_UUID_SIZE);
        HashValue(high, cacheVersion);
        HashValue(low, cacheVersion);
        HashBytes(low, properties.pipelineCacheUUID, VK_UUID_SIZE);
        HashValue(low, properties.deviceID);
        HashValue(low, properties.vendorID);
        HashValue(low, backend);
        return {.high = high, .low = low};
    }
}

Allocator* VulkanDevice::GetAllocator() const { return mApp.GetAllocator(); }

VulkanDevice::VulkanDevice(VulkanApplication const& app, vk::raii::PhysicalDevice physicalDevice, SDL_Window* window) :
    RHIDevice(app), mApp(app), mWindow(window), mPhysicalDevice(std::move(physicalDevice)),
    mSwapchainFormats(GetAllocator()), mSwapchainPresentModes(GetAllocator()), mStorage(GetAllocator())
{
    mPhysicalDeviceProperties = mPhysicalDevice.getProperties();
    mPipelineCacheKey = MakePipelineCacheKey(mPhysicalDeviceProperties);
    auto families = mPhysicalDevice.getQueueFamilyProperties();
    // Find queues
    // Graphics, Compute, Transfer should be preferably mutually exclusive
    // vvv [Family, Index]
    Vector<uint32_t> vis(families.size(), GetAllocator());
    using QueuePair = Pair<uint32_t, uint32_t>;
    QueuePair graphics{kInvalidQueueIndex, 0}, compute{kInvalidQueueIndex, 0}, transfer{kInvalidQueueIndex, 0};
    Pair<QueuePair*, vk::QueueFlagBits> dstPairs[] = {
        {&graphics, vk::QueueFlagBits::eGraphics},
        {&compute, vk::QueueFlagBits::eCompute},
        {&transfer, vk::QueueFlagBits::eTransfer},
    };
    for (auto& [dstPair, flag] : dstPairs)
    {
        for (int i = 0; i < families.size(); ++i)
        {
            auto& family = families[i];
            if (family.queueCount && (family.queueFlags & flag) && !vis[i])
            {
                dstPair->first = i;
                dstPair->second = vis[i]++;
                family.queueCount--;
                break;
            }
        }
    }
    // Unassigned - use remaining queues
    // Create the device queues
    for (auto& [dstPair, flag] : dstPairs)
    {
        if (dstPair->first != kInvalidQueueIndex)
            continue;
        for (int i = 0; i < families.size(); ++i)
        {
            auto& family = families[i];
            if (family.queueCount && (family.queueFlags & flag))
            {
                dstPair->first = i;
                dstPair->second = vis[i]++;
                family.queueCount--;
                break;
            }
        }
    }
    Vector<vk::DeviceQueueCreateInfo> queueInfos(GetAllocator());
    static const float priority[16] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                       1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    for (uint32_t i = 0; i < families.size(); ++i)
    {
        if (vis[i])
            queueInfos.emplace_back(vk::DeviceQueueCreateInfo{
                .queueFamilyIndex = i,
                .queueCount = vis[i],
                .pQueuePriorities = priority // All queues have the same priority
            });
    }
    CHECK(graphics.first != kInvalidQueueIndex);
    // Fallback to graphics queue
    if (compute.first == kInvalidQueueIndex)
        compute = graphics;
    if (transfer.first == kInvalidQueueIndex)
        transfer = graphics;
    if (window)
    {
        // Check for a present queue
        VkSurfaceKHR surface;
        CHECK_MSG(SDL_Vulkan_CreateSurface(window, *mApp.GetVkInstance(),
                      mApp.GetVkAllocationCallbacksNative(), &surface),
                  "failed to create window surface: {}", SDL_GetError());
        mSurface = vk::raii::SurfaceKHR(mApp.GetVkInstance(), surface, mApp.GetVkAllocationCallbacks());
        // Having present and graphics queues as the same avoids copies and is typically the case
        // - https://github.com/KhronosGroup/Vulkan-Hpp/blob/main/RAII_Samples/05_InitSwapchain/05_InitSwapchain.cpp#L45
        // - https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/master/src/VulkanSample.cpp#L1850
        CHECK(mPhysicalDevice.getSurfaceSupportKHR(graphics.first, *mSurface));
    }
// Define the type once so you aren't copy-pasting the template list everywhere.
    using DeviceFeatureChain = vk::StructureChain<
        vk::PhysicalDeviceFeatures2,
        vk::PhysicalDeviceVulkan11Features,
        vk::PhysicalDeviceVulkan12Features,
        vk::PhysicalDeviceVulkan13Features,
        vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT,
        vk::PhysicalDeviceMeshShaderFeaturesEXT,
        vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
        vk::PhysicalDeviceRayQueryFeaturesKHR,
        vk::PhysicalDeviceRayTracingPipelineFeaturesKHR,
        vk::PhysicalDeviceRayTracingInvocationReorderFeaturesEXT>;

    // featureChain is zero-initialized (everything defaults to false).
    // supportedChain will hold what the physical device actually supports.
    DeviceFeatureChain featureChain{};
    DeviceFeatureChain supportedChain{};

    // --- Extension Querying ---
    auto availableExtensions = mPhysicalDevice.enumerateDeviceExtensionProperties();
    auto isExtensionAvailable = [&](const char* extName) -> bool {
        for (auto const& ext : availableExtensions) {
            if (std::string_view(ext.extensionName.data()) == extName) return true;
        }
        return false;
    };

    Vector<const char*> enabledExtensions(GetAllocator());
    for (auto* desired : kVulkanDesiredDeviceExtensions) {
        if (isExtensionAvailable(desired)) {
            enabledExtensions.push_back(desired);
        } else {
            LOG(VulkanDevice, LogWarn, "Device extension '{}' is not available and will not be enabled.", desired);
        }
    }

    auto isExtensionEnabled = [&](const char* extName) -> bool {
        for (auto* ext : enabledExtensions) {
            if (std::string_view(ext) == extName) return true;
        }
        return false;
    };

    const bool hasMeshShader = isExtensionEnabled(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    const bool hasAccelerationStructure = isExtensionEnabled(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    const bool hasRayQuery = isExtensionEnabled(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    const bool hasRayTracingPipeline = isExtensionEnabled(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    const bool hasRayTracingInvocationReorder = isExtensionEnabled(VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME);
    #define UNLINK_IF_UNSUPPORTED(FLAG, STRUCT_TYPE) \
        if (!(FLAG)) { \
            featureChain.unlink<STRUCT_TYPE>(); \
            supportedChain.unlink<STRUCT_TYPE>(); \
        }
    UNLINK_IF_UNSUPPORTED(hasMeshShader, vk::PhysicalDeviceMeshShaderFeaturesEXT)
    UNLINK_IF_UNSUPPORTED(hasAccelerationStructure, vk::PhysicalDeviceAccelerationStructureFeaturesKHR)
    UNLINK_IF_UNSUPPORTED(hasRayQuery, vk::PhysicalDeviceRayQueryFeaturesKHR)
    UNLINK_IF_UNSUPPORTED(hasRayTracingPipeline, vk::PhysicalDeviceRayTracingPipelineFeaturesKHR)
    UNLINK_IF_UNSUPPORTED(hasRayTracingInvocationReorder, vk::PhysicalDeviceRayTracingInvocationReorderFeaturesEXT)
    #undef UNLINK_IF_UNSUPPORTED
    (*mPhysicalDevice).getFeatures2(&supportedChain.get<vk::PhysicalDeviceFeatures2>());

    // Special case for VkPhysicalDeviceFeatures since it's nested inside PhysicalDeviceFeatures2.features
    #define REQUEST_BASE_FEATURE(FEATURE_MEMBER) \
        if (supportedChain.get<vk::PhysicalDeviceFeatures2>().features.FEATURE_MEMBER) { \
            featureChain.get<vk::PhysicalDeviceFeatures2>().features.FEATURE_MEMBER = VK_TRUE; \
        } else { \
            LOG(VulkanDevice, LogWarn, "Device feature 'VkPhysicalDeviceFeatures::" #FEATURE_MEMBER "' is not supported and will remain disabled."); \
        }
    // Base Features
    REQUEST_BASE_FEATURE(samplerAnisotropy)
    REQUEST_BASE_FEATURE(fragmentStoresAndAtomics)
    REQUEST_BASE_FEATURE(shaderInt16)

    #define REQUEST_FEATURE(STRUCT_TYPE, FEATURE_MEMBER) \
        if (supportedChain.get<STRUCT_TYPE>().FEATURE_MEMBER) { \
            featureChain.get<STRUCT_TYPE>().FEATURE_MEMBER = VK_TRUE; \
        } else { \
            LOG(VulkanDevice, LogWarn, "Device feature '" #STRUCT_TYPE "::" #FEATURE_MEMBER "' is not supported and will remain disabled."); \
        }

    // Vulkan 1.1 Features
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan11Features, storageBuffer16BitAccess)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan11Features, uniformAndStorageBuffer16BitAccess)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan11Features, shaderDrawParameters)

    // Vulkan 1.2 Features
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan12Features, drawIndirectCount)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan12Features, storageBuffer8BitAccess)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan12Features, uniformAndStorageBuffer8BitAccess)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan12Features, shaderFloat16)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan12Features, shaderInt8)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan12Features, descriptorIndexing)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan12Features, descriptorBindingSampledImageUpdateAfterBind)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan12Features, runtimeDescriptorArray)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan12Features, samplerFilterMinmax)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan12Features, scalarBlockLayout)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan12Features, uniformBufferStandardLayout)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan12Features, shaderSubgroupExtendedTypes)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan12Features, hostQueryReset)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan12Features, timelineSemaphore)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan12Features, bufferDeviceAddress)

    // Vulkan 1.3 Features
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan13Features, synchronization2)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan13Features, dynamicRendering)
    REQUEST_FEATURE(vk::PhysicalDeviceVulkan13Features, shaderIntegerDotProduct)

    // Extended Dynamic State
    REQUEST_FEATURE(vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT, extendedDynamicState)

    // Optional Extension Features (Only checked if the extension was actually enabled)
    if (hasMeshShader) {
        REQUEST_FEATURE(vk::PhysicalDeviceMeshShaderFeaturesEXT, meshShader)
        // You specifically didn't want taskShader enabled in your original snippet,
        // so we simply don't request it here. It stays false.
    }

    if (hasAccelerationStructure) {
        REQUEST_FEATURE(vk::PhysicalDeviceAccelerationStructureFeaturesKHR, accelerationStructure)
    }

    if (hasRayQuery) {
        REQUEST_FEATURE(vk::PhysicalDeviceRayQueryFeaturesKHR, rayQuery)
    }

    if (hasRayTracingPipeline) {
        REQUEST_FEATURE(vk::PhysicalDeviceRayTracingPipelineFeaturesKHR, rayTracingPipeline)
    }

    bool shaderExecutionReordering = false;
    if (hasRayTracingInvocationReorder) {
        REQUEST_FEATURE(vk::PhysicalDeviceRayTracingInvocationReorderFeaturesEXT, rayTracingInvocationReorder)
        shaderExecutionReordering =
            featureChain.get<vk::PhysicalDeviceRayTracingInvocationReorderFeaturesEXT>().rayTracingInvocationReorder ==
            VK_TRUE;
    }

    vk::DeviceCreateInfo device_info{.pNext = &featureChain.get<vk::PhysicalDeviceFeatures2>(),
                                     .queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size()),
                                     .pQueueCreateInfos = queueInfos.data(),
                                     .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
                                     .ppEnabledExtensionNames = enabledExtensions.data()};
    mDevice = vk::raii::Device(mPhysicalDevice, device_info, GetVkAllocationCallbacks());
    CHECK(mDevice != nullptr && "failed to create Vulkan device");
    // Allocate the queues
    mQueues = ConstructUnique<VulkanDeviceQueues>(GetAllocator(), GetAllocator());
    mQueues->graphics = mQueues->storage.CreateObject<VulkanDeviceQueue>(*this, graphics.first, graphics.second);
    mQueues->compute = mQueues->storage.CreateObject<VulkanDeviceQueue>(*this, compute.first, compute.second);
    mQueues->transfer = mQueues->storage.CreateObject<VulkanDeviceQueue>(*this, transfer.first, transfer.second);
    // Initialize VMA
    const VmaAllocatorCreateInfo allocator_info{
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice = *mPhysicalDevice,
        .device = *mDevice,
        .pAllocationCallbacks = GetVkAllocationCallbacksNative(),
        .instance = *mApp.GetVkInstance(),
        .vulkanApiVersion = mApp.mVulkanApiVersion};
    CHECK(vmaCreateAllocator(&allocator_info, &mVkAllocator) == VK_SUCCESS && "failed to create VMA for Vulkan device");
    if (mSurface != nullptr)
    {
        // Collect swapchain (surface) info
        auto formats = mPhysicalDevice.getSurfaceFormatsKHR(mSurface);
        for (auto& fmt : formats)
        {
            using enum RHIResourceFormat;
            using enum RHIColorSpace;
            RHIResourceFormat rhiFormat = Undefined;
            switch (fmt.format)
            {
            case vk::Format::eR8G8B8A8Unorm:
                rhiFormat = R8G8B8A8Unorm;
                break;
            case vk::Format::eR8G8B8A8Srgb:
                rhiFormat = R8G8B8A8Srgb;
                break;
            case vk::Format::eB8G8R8A8Unorm:
                rhiFormat = B8G8R8A8Unrom;
                break;
            case vk::Format::eB8G8R8A8Srgb:
                rhiFormat = B8G8R8A8Srgb;
                break;
            case vk::Format::eA2B10G10R10UnormPack32:
                rhiFormat = A2B10G10R10Unorm;
                break;
            case vk::Format::eA2B10G10R10SnormPack32:
                rhiFormat = A2B10G10R10Snorm;
                break;
            case vk::Format::eA2R10G10B10UnormPack32:
                rhiFormat = A2R10G10B10Unorm;
                break;
            case vk::Format::eA2R10G10B10SnormPack32:
                rhiFormat = A2R10G10B10Snorm;
                break;
            default:
                // TODO: More formats? HDR?
                break;
            }
            if (rhiFormat != Undefined)
            {
                mSwapchainFormats.emplace_back(RHISurfaceFormat{rhiFormat, rhiColorSpaceFromVkColorSpace(fmt.colorSpace)});
            }
        }
        auto modes = mPhysicalDevice.getSurfacePresentModesKHR(mSurface);
        for (auto& mode : modes)
        {
            using enum RHISwapchainPresentMode;
            switch (mode)
            {
            case vk::PresentModeKHR::eMailbox:
                mSwapchainPresentModes.emplace_back(Mailbox);
                break;
            case vk::PresentModeKHR::eImmediate:
                mSwapchainPresentModes.emplace_back(Tearing);
                break;
            case vk::PresentModeKHR::eFifo:
                mSwapchainPresentModes.emplace_back(Fifo);
                break;
            default:
                break;
            }
        }
    }
    auto properties = mPhysicalDevice.getProperties();
    auto memoryProperties = mPhysicalDevice.getMemoryProperties();
    bool deviceLocalHostVisibleBuffers = false;
    size_t deviceLocalHostVisibleHeapSize = 0;
    Bitset<VK_MAX_MEMORY_HEAPS> deviceLocalHostVisibleHeaps{};
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
    {
        auto const& memoryType = memoryProperties.memoryTypes[i];
        auto flags = memoryType.propertyFlags;
        if ((flags & vk::MemoryPropertyFlagBits::eDeviceLocal) &&
            (flags & vk::MemoryPropertyFlagBits::eHostVisible))
        {
            deviceLocalHostVisibleBuffers = true;
            deviceLocalHostVisibleHeaps[memoryType.heapIndex] = true;
        }
    }
    for (uint32_t i = 0; i < memoryProperties.memoryHeapCount; ++i)
    {
        if (deviceLocalHostVisibleHeaps[i])
            deviceLocalHostVisibleHeapSize += memoryProperties.memoryHeaps[i].size;
    }
    // Assume supported if and only if all queues can do timestamp queries
    const bool timestampQueries = properties.limits.timestampPeriod != 0.0f &&
        families[graphics.first].timestampValidBits != 0 &&
        families[compute.first].timestampValidBits != 0;
    // Fill in device capabilities
    mDeviceCaps = {
        .dedicatedCompute = mQueues->graphics != mQueues->compute,
        .dedicatedTransfer = mQueues->graphics != mQueues->transfer,
        .shaderExecutionReordering = shaderExecutionReordering,
        .meshShaders = hasMeshShader,
        .raytracingInline = hasRayQuery,
        .raytracingPipeline = hasRayTracingPipeline,
        .timestampQueries = timestampQueries,
        .integratedGPU = properties.deviceType == vk::PhysicalDeviceType::eIntegratedGpu,
        .deviceLocalHostVisibleBuffers = deviceLocalHostVisibleBuffers,
        .deviceLocalHostVisibleHeapSize = deviceLocalHostVisibleHeapSize,
        .maxStorageBufferRange = properties.limits.maxStorageBufferRange,
    };
}

VulkanDevice::~VulkanDevice()
{
    mStorage.Destroy();
    if (mVkAllocator)
    {
        vmaDestroyAllocator(mVkAllocator);
        mVkAllocator = nullptr;
    }
    mQueues.reset();
    mSurface = nullptr;
    mDevice = nullptr;
    mPhysicalDevice = nullptr;
}

void VulkanDevice::WaitIdle() const { mDevice.waitIdle(); }

void VulkanDevice::QueryBudget(size_t& used, size_t& budget) const
{
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(mVkAllocator, budgets);
    used = 0, budget = 0;
    for (const auto& b : budgets)
        used += b.usage, budget += b.budget;
}

void VulkanDevice::QueryAllocationStats(size_t& blockBytes, size_t& allocationBytes) const
{
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(mVkAllocator, budgets);
    blockBytes = 0, allocationBytes = 0;
    for (const auto& b : budgets)
    {
        blockBytes += b.statistics.blockBytes;
        allocationBytes += b.statistics.allocationBytes;
    }
}

void VulkanDevice::QueryMemoryStats(RHIDeviceMemoryStats& outStats) const
{
    outStats.heaps.clear();
    outStats.memoryTypes.clear();
    outStats.total = {};

    auto memoryProperties = mPhysicalDevice.getMemoryProperties();

    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(mVkAllocator, budgets);

    VmaTotalStatistics stats{};
    vmaCalculateStatistics(mVkAllocator, &stats);

    auto FillDetailedStats = [](RHIDeviceMemoryTypeStat& dst, VmaDetailedStatistics const& src)
    {
        dst.blockCount = src.statistics.blockCount;
        dst.allocationCount = src.statistics.allocationCount;
        dst.unusedRangeCount = src.unusedRangeCount;
        dst.blockBytes = static_cast<size_t>(src.statistics.blockBytes);
        dst.allocationBytes = static_cast<size_t>(src.statistics.allocationBytes);
        dst.allocationSizeMin = src.statistics.allocationCount ? static_cast<size_t>(src.allocationSizeMin) : 0;
        dst.allocationSizeMax = static_cast<size_t>(src.allocationSizeMax);
        dst.unusedRangeSizeMin = src.unusedRangeCount ? static_cast<size_t>(src.unusedRangeSizeMin) : 0;
        dst.unusedRangeSizeMax = static_cast<size_t>(src.unusedRangeSizeMax);
    };

    outStats.heaps.reserve(memoryProperties.memoryHeapCount);
    for (uint32_t i = 0; i < memoryProperties.memoryHeapCount; ++i)
    {
        auto const& heap = memoryProperties.memoryHeaps[i];
        auto const& budget = budgets[i];
        outStats.heaps.push_back({
            .heapIndex = i,
            .deviceLocal = static_cast<bool>(heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal),
            .heapSize = static_cast<size_t>(heap.size),
            .usage = static_cast<size_t>(budget.usage),
            .budget = static_cast<size_t>(budget.budget),
            .blockCount = budget.statistics.blockCount,
            .allocationCount = budget.statistics.allocationCount,
            .blockBytes = static_cast<size_t>(budget.statistics.blockBytes),
            .allocationBytes = static_cast<size_t>(budget.statistics.allocationBytes),
        });
    }

    outStats.memoryTypes.reserve(memoryProperties.memoryTypeCount);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
    {
        auto const& memoryType = memoryProperties.memoryTypes[i];
        auto flags = memoryType.propertyFlags;
        RHIDeviceMemoryTypeStat stat{
            .typeIndex = i,
            .heapIndex = memoryType.heapIndex,
            .deviceLocal = static_cast<bool>(flags & vk::MemoryPropertyFlagBits::eDeviceLocal),
            .hostVisible = static_cast<bool>(flags & vk::MemoryPropertyFlagBits::eHostVisible),
            .hostCoherent = static_cast<bool>(flags & vk::MemoryPropertyFlagBits::eHostCoherent),
            .hostCached = static_cast<bool>(flags & vk::MemoryPropertyFlagBits::eHostCached),
            .lazilyAllocated = static_cast<bool>(flags & vk::MemoryPropertyFlagBits::eLazilyAllocated),
            .protectedMemory = static_cast<bool>(flags & vk::MemoryPropertyFlagBits::eProtected),
        };
        FillDetailedStats(stat, stats.memoryType[i]);
        outStats.memoryTypes.push_back(stat);
    }

    FillDetailedStats(outStats.total, stats.total);
}

String VulkanDevice::QueryDeviceString() const
{
    auto properties = mPhysicalDevice.getProperties();
    return fmt::format("{} ({}) on Vulkan {}.{}.{}", &properties.deviceName[0],
                       kVulkanDeviceTypes[static_cast<size_t>(properties.deviceType)],
                       VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
                       VK_VERSION_PATCH(properties.apiVersion));
}

VulkanDeviceQueue* VulkanDeviceQueues::Get(Handle handle) const { return storage.GetObjectPtr(handle); }

RHIDeviceQueue* VulkanDevice::GetDeviceQueue(RHIDeviceQueueType type) const
{
    switch (type)
    {
    case RHIDeviceQueueType::Graphics:
        return mQueues->Get(mQueues->graphics);
    case RHIDeviceQueueType::Compute:
        return mQueues->Get(mQueues->compute);
    case RHIDeviceQueueType::Transfer:
        return mQueues->Get(mQueues->transfer);
    default:
        break;
    }
    return nullptr;
}

#include "Swapchain.hpp"
Span<RHISurfaceFormat const> VulkanDevice::GetSwapchainSupportedFormats() const { return mSwapchainFormats; }

Span<RHISwapchainPresentMode const> VulkanDevice::GetSwapchainSupportedPresentModes() const
{
    return mSwapchainPresentModes;
}

RHIDeviceScopedHandle<RHISwapchain> VulkanDevice::CreateSwapchain(RHISwapchain::SwapchainDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanSwapchain>(*this, desc)};
}

RHISwapchain* VulkanDevice::GetSwapchain(Handle handle) const { return mStorage.GetObjectPtr<RHISwapchain>(handle); };
void VulkanDevice::DestroySwapchain(Handle handle) { mStorage.DestroyObject(handle); }

RHIDeviceScopedHandle<RHIPipelineStateCache>
VulkanDevice::CreatePipelineCache(RHIPipelineStateCache::PipelineStateCacheDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanPipelineStateCache>(*this, desc)};
}

RHIPipelineStateCache* VulkanDevice::GetPipelineCache(Handle handle) const
{
    return mStorage.GetObjectPtr<RHIPipelineStateCache>(handle);
}

void VulkanDevice::DestroyPipelineCache(Handle handle) { mStorage.DestroyObject(handle); }

#include "PipelineState.hpp"

RHIDeviceScopedHandle<RHIPipelineState>
VulkanDevice::CreatePipelineState(RHIPipelineState::PipelineStateDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanPipelineState>(*this, desc)};
}

RHIPipelineState* VulkanDevice::GetPipelineState(Handle handle) const
{
    return mStorage.GetObjectPtr<RHIPipelineState>(handle);
}

void VulkanDevice::DestroyPipelineState(Handle handle) { mStorage.DestroyObject(handle); }

#include "Shader.hpp"

RHIDeviceScopedHandle<RHIShaderModule> VulkanDevice::CreateShaderModule(RHIShaderModule::ShaderModuleDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanShaderModule>(*this, desc)};
}

RHIShaderModule* VulkanDevice::GetShaderModule(Handle handle) const
{
    return mStorage.GetObjectPtr<RHIShaderModule>(handle);
}

void VulkanDevice::DestroyShaderModule(Handle handle) { mStorage.DestroyObject(handle); }

#include "Command.hpp"

RHIDeviceScopedHandle<RHICommandPool> VulkanDevice::CreateCommandPool(RHICommandPool::PoolDesc desc)
{
    return {this, mStorage.CreateObject<VulkanCommandPool>(*this, desc, GetAllocator())};
}

RHICommandPool* VulkanDevice::GetCommandPool(Handle handle) const
{
    return mStorage.GetObjectPtr<RHICommandPool>(handle);
}

void VulkanDevice::DestroyCommandPool(Handle handle) { mStorage.DestroyObject(handle); }

VulkanDeviceSemaphore::VulkanDeviceSemaphore(const VulkanDevice& device, bool is_timeline) :
    RHIDeviceSemaphore(device, is_timeline), mDevice(device)
{
    vk::SemaphoreCreateInfo info{};
    vk::SemaphoreTypeCreateInfo tinfo{.semaphoreType = vk::SemaphoreType::eTimeline, .initialValue = 0};
    if (is_timeline)
        info.setPNext(&tinfo);
    mSemaphore = vk::raii::Semaphore(mDevice.GetVkDevice(), info, mDevice.GetVkAllocationCallbacks());
}

void VulkanDeviceSemaphore::DebugSetObjectName(const char* name)
{
    VkSemaphore handle = *mSemaphore;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eSemaphore,
                                                      .objectHandle = reinterpret_cast<uint64_t>(handle),
                                                      .pObjectName = name});
}

VulkanDeviceFence::VulkanDeviceFence(const VulkanDevice& device, bool signaled) :
    RHIDeviceFence(device), mDevice(device),
    mFence(vk::raii::Fence(
        device.GetVkDevice(),
        vk::FenceCreateInfo{.flags = signaled ? vk::FenceCreateFlagBits::eSignaled : vk::FenceCreateFlags{}},
        device.GetVkAllocationCallbacks()))
{
}

void VulkanDeviceFence::DebugSetObjectName(const char* name)
{
    VkFence handle = *mFence;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eFence,
                                                      .objectHandle = reinterpret_cast<uint64_t>(handle),
                                                      .pObjectName = name});
}

RHIDeviceScopedHandle<RHIDeviceSemaphore> VulkanDevice::CreateSemaphore(bool is_timeline)
{
    return {this, mStorage.CreateObject<VulkanDeviceSemaphore>(*this, is_timeline)};
}

RHIDeviceSemaphore* VulkanDevice::GetSemaphore(Handle handle) const
{
    return mStorage.GetObjectPtr<RHIDeviceSemaphore>(handle);
}

void VulkanDevice::DestroySemaphore(Handle handle) { mStorage.DestroyObject(handle); }

auto VulkanDevice::CreateFence(bool signaled) -> RHIDeviceScopedHandle<RHIDeviceFence>
{
    return {this, mStorage.CreateObject<VulkanDeviceFence>(*this, signaled)};
}

RHIDeviceFence* VulkanDevice::GetFence(Handle handle) const { return mStorage.GetObjectPtr<RHIDeviceFence>(handle); }
void VulkanDevice::DestroyFence(Handle handle) { mStorage.DestroyObject(handle); }

void VulkanDevice::ResetFences(Span<RHIDeviceFence* const> fences)
{
    StackArena arena;
    AllocatorStack alloc(arena);
    Vector<vk::Fence> vk_fences(alloc.Ptr());
    vk_fences.reserve(fences.size());
    for (auto* fence : fences)
        vk_fences.emplace_back(static_cast<VulkanDeviceFence*>(fence)->GetVkFence());
    mDevice.resetFences(vk_fences);
}

bool VulkanDevice::WaitForFences(Span<RHIDeviceFence* const> fences, bool wait_all, size_t timeout)
{
    StackArena arena;
    AllocatorStack alloc(arena);
    Vector<vk::Fence> vk_fences(alloc.Ptr());
    vk_fences.reserve(fences.size());
    for (auto const& fence : fences)
        vk_fences.emplace_back(static_cast<VulkanDeviceFence*>(fence)->GetVkFence());
    vk::Result res = mDevice.waitForFences(vk_fences, wait_all, timeout);
    if (res == vk::Result::eSuccess)
        return true;
    if (res == vk::Result::eTimeout)
        return false;
    CHECK_MSG(false, "failed to wait for fence. result={}", vk::to_string(res));
}

void VulkanDevice::SignalTimelineSemaphores(Span<const Pair<RHIDeviceSemaphore*, size_t>> semaphores)
{
    for (auto const& [signal, val] : semaphores)
    {
        CHECK(signal->mIsTimeline);
        vk::SemaphoreSignalInfo info{.semaphore = static_cast<VulkanDeviceSemaphore*>(signal)->GetVkSemaphore(),
                                     .value = val};
        mDevice.signalSemaphore(info);
    }
}

bool VulkanDevice::WaitForTimelineSemaphores(Span<const Pair<RHIDeviceSemaphore*, size_t>> semaphores, size_t timeout)
{
    StackArena arena{};
    AllocatorStack alloc(arena);
    Vector<vk::Semaphore> vk_semaphores(alloc.Ptr());
    Vector<uint64_t> vk_values(alloc.Ptr());
    vk_semaphores.reserve(semaphores.size()), vk_values.reserve(semaphores.size());
    for (auto const& [wait, val] : semaphores)
    {
        CHECK(wait->mIsTimeline);
        vk_semaphores.emplace_back(static_cast<VulkanDeviceSemaphore*>(wait)->GetVkSemaphore()),
            vk_values.emplace_back(val);
    }
    auto res =
        mDevice.waitSemaphores(vk::SemaphoreWaitInfo{.semaphoreCount = static_cast<uint32_t>(vk_semaphores.size()),
                                                     .pSemaphores = vk_semaphores.data(),
                                                     .pValues = vk_values.data()},
                               timeout);
    if (res == vk::Result::eSuccess)
        return true;
    if (res == vk::Result::eTimeout)
        return false;
    CHECK_MSG(false, "failed to wait for semaphores. result={}", vk::to_string(res));
}

vk::AllocationCallbacks const* VulkanDevice::GetVkAllocationCallbacks() const
{
    return mApp.GetVkAllocationCallbacks();
}
VkAllocationCallbacks const* VulkanDevice::GetVkAllocationCallbacksNative() const
{
    return mApp.GetVkAllocationCallbacksNative();
}
void VulkanDevice::DebugSetObjectName(const char* name)
{
    VkDevice handle = *mDevice;
    mDevice.setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eDevice,
                                        .objectHandle = reinterpret_cast<uint64_t>(handle),
                                        .pObjectName = name});
}

void VulkanDeviceQueue::WaitIdle() const
{
    std::lock_guard<Mutex> submitLock(mDevice.GetQueueSubmitMutex());
    mQueue.waitIdle();
}

void VulkanDeviceQueue::Submit(Span<const SubmitDesc> descs, RHIDeviceFence* completionFence) const
{
    StackArena<4096> arena{};
    AllocatorStack alloc(arena);
    Vector<vk::SubmitInfo> submits(alloc.Ptr());
    submits.reserve(descs.size());
    for (auto const& desc : descs)
    {
        auto cmds = ConstructSpan<vk::CommandBuffer>(alloc.Ptr(), desc.cmdLists.size());
        auto swaits = ConstructSpan<vk::Semaphore>(alloc.Ptr(), desc.timelineWaits.size() + desc.waits.size());
        auto ssignals = ConstructSpan<vk::Semaphore>(alloc.Ptr(), desc.timelineSignals.size() + desc.signals.size());
        auto wait_values = ConstructSpan<uint64_t>(alloc.Ptr(), swaits.size());
        auto signal_values = ConstructSpan<uint64_t>(alloc.Ptr(), ssignals.size());
        auto* pcmd = cmds.data();
        for (auto const& cmd_list : desc.cmdLists)
            *pcmd++ = static_cast<VulkanCommandList*>(cmd_list)->GetVkCommandBuffer();
        auto* pswaits = swaits.data();
        auto* pwait_values = wait_values.data();
        for (auto const& [wait, val] : desc.timelineWaits)
        {
            CHECK(wait->mIsTimeline && "Submit() timeline_signals must be Timeline semaphores");
            *pswaits++ = static_cast<VulkanDeviceSemaphore*>(wait)->GetVkSemaphore();
            *pwait_values++ = val;
        }
        auto* pssignals = ssignals.data();
        auto* psignal_values = signal_values.data();
        for (auto const& [signal, val] : desc.timelineSignals)
        {
            CHECK(signal->mIsTimeline && "Submit() timeline_signals must be Timeline semaphores");
            *pssignals++ = static_cast<VulkanDeviceSemaphore*>(signal)->GetVkSemaphore();
            *psignal_values++ = val;
        }
        for (auto const& wait : desc.waits)
        {
            CHECK(!wait->mIsTimeline && "Submit() waits must be Binary semaphores");
            *pswaits++ = static_cast<VulkanDeviceSemaphore*>(wait)->GetVkSemaphore();
            // https://registry.khronos.org/vulkan/specs/latest/man/html/VkTimelineSemaphoreSubmitInfo.html#_description
            // Driver should ignore these
            *pwait_values++ = 0;
        }
        for (auto const& signal : desc.signals)
        {
            CHECK(!signal->mIsTimeline && "Submit() signals must be Binary semaphores");
            *pssignals++ = static_cast<VulkanDeviceSemaphore*>(signal)->GetVkSemaphore();
            *psignal_values++ = 0;
        }
        CHECK_MSG(desc.waitsStages.size() == swaits.size(),
                  "Number of wait stages ({}) must match number of wait (timeline+binary) semaphores ({})",
                  desc.waitsStages.size(), swaits.size());
        auto stages = ConstructSpan<vk::PipelineStageFlags>(alloc.Ptr(), desc.waitsStages.size());
        auto* pstages = stages.data();
        for (auto stage : desc.waitsStages)
            *pstages++ = vkPipelineStageFlagsFromRHIPipelineStage(stage);
        vk::SubmitInfo info{.waitSemaphoreCount = static_cast<uint32_t>(swaits.size()),
                            .pWaitSemaphores = swaits.data(),
                            .pWaitDstStageMask = stages.data(),
                            .commandBufferCount = static_cast<uint32_t>(cmds.size()),
                            .pCommandBuffers = cmds.data(),
                            .signalSemaphoreCount = static_cast<uint32_t>(ssignals.size()),
                            .pSignalSemaphores = ssignals.data()};
        auto* tinfo = Construct<vk::TimelineSemaphoreSubmitInfo>(alloc.Ptr());
        *tinfo =
            vk::TimelineSemaphoreSubmitInfo{.waitSemaphoreValueCount = static_cast<uint32_t>(wait_values.size()),
                                            .pWaitSemaphoreValues = wait_values.data(),
                                            .signalSemaphoreValueCount = static_cast<uint32_t>(signal_values.size()),
                                            .pSignalSemaphoreValues = signal_values.data()};
        if (!wait_values.empty() || !signal_values.empty())
            info.setPNext(tinfo);
        submits.push_back(info);
    }
    std::lock_guard<Mutex> submitLock(mDevice.GetQueueSubmitMutex());
    mQueue.submit(
        submits, completionFence ? static_cast<VulkanDeviceFence*>(completionFence)->GetVkFence() : vk::Fence(nullptr));
}

void VulkanDeviceQueue::Present(PresentDesc const& desc) const
{
    StackArena<4096> arena{};
    AllocatorStack alloc(arena);
    vk::SwapchainKHR swapchain = static_cast<VulkanSwapchain*>(desc.swapchain)->GetVkSwapchain();
    Vector<vk::Semaphore> swaits(alloc.Ptr());
    swaits.reserve(desc.waits.size());
    for (auto& wait : desc.waits)
    {
        CHECK(!wait->mIsTimeline && "Present() wait must be Binary semaphores");
        swaits.emplace_back(static_cast<const VulkanDeviceSemaphore*>(wait)->GetVkSemaphore());
    }
    vk::PresentInfoKHR present_info{
        .waitSemaphoreCount = static_cast<uint32_t>(swaits.size()),
        .pWaitSemaphores = swaits.data(),
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &desc.imageIndex,
    };
    try
    {
        std::lock_guard<Mutex> submitLock(mDevice.GetQueueSubmitMutex());
        auto res = mQueue.presentKHR(present_info);
        CHECK(res == vk::Result::eSuccess && "failed to present");
    }
    catch (std::exception&)
    {
        // XXX: Not always the case e.g. device lost
        throw RHISwapchainResizeException{};
    }
}

void VulkanDeviceQueue::DebugSetObjectName(const char* name)
{
    VkQueue handle = *mQueue;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eQueue,
                                                      .objectHandle = reinterpret_cast<uint64_t>(handle),
                                                      .pObjectName = name});
}

RHIDeviceScopedHandle<RHIBuffer> VulkanDevice::CreateBuffer(RHIBufferDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanBuffer>(*this, desc)};
}

RHIBuffer* VulkanDevice::GetBuffer(Handle handle) const { return mStorage.GetObjectPtr<RHIBuffer>(handle); }
void VulkanDevice::DestroyBuffer(Handle handle) { mStorage.DestroyObject(handle); }

RHIDeviceScopedHandle<RHITexture> VulkanDevice::CreateTexture(RHITextureDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanTexture>(*this, desc)};
}

RHITexture* VulkanDevice::GetTexture(Handle handle) const { return mStorage.GetObjectPtr<RHITexture>(handle); }
void VulkanDevice::DestroyTexture(Handle handle) { mStorage.DestroyObject(handle); }

RHIAccelerationStructureSizeInfo
VulkanDevice::GetAccelerationStructureSizeInfo(RHIAccelerationStructureBuildDesc const& desc,
                                               Allocator* scratchAllocator) const
{
    auto* alloc = scratchAllocator ? scratchAllocator : GetAllocator();
    Vector<vk::AccelerationStructureGeometryKHR> geos(alloc);
    Vector<uint32_t> primitiveCounts(alloc);
    auto buildInfo = vkAccelerationBuildGeoInfoFromRHI(desc, geos, primitiveCounts);
    vk::AccelerationStructureBuildSizesInfoKHR sizeInfo = mDevice.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice, buildInfo, primitiveCounts);
    return {.accelerationStructureSize = static_cast<uint32_t>(sizeInfo.accelerationStructureSize),
            .buildScratchSize = static_cast<uint32_t>(sizeInfo.buildScratchSize),
            .updateScratchSize = static_cast<uint32_t>(sizeInfo.updateScratchSize)};
}

size_t VulkanDevice::WriteAccelerationStructureInstanceData(RHIAccelerationStructureGeometryInstance const& data,
                                                            void* dest) const
{
    if (dest)
    {
        vk::AccelerationStructureInstanceKHR res{
            .instanceCustomIndex = static_cast<uint32_t>(data.instanceID),
            .mask = data.mask,
            .instanceShaderBindingTableRecordOffset = data.shaderBindingTableRecordOffset,
            .accelerationStructureReference =
            static_cast<const VulkanAccelerationStructure*>(data.blas)->GetVkAccelerationStructureAddress()};
        std::memcpy(res.transform.matrix[0], &data.transformBasisRowMajor[0], sizeof(float) * 3);
        std::memcpy(res.transform.matrix[1], &data.transformBasisRowMajor[1], sizeof(float) * 3);
        std::memcpy(res.transform.matrix[2], &data.transformBasisRowMajor[2], sizeof(float) * 3);
        res.transform.matrix[0][3] = data.transformTranslation[0];
        res.transform.matrix[1][3] = data.transformTranslation[1];
        res.transform.matrix[2][3] = data.transformTranslation[2];
        if (data.flags & RHIAccelerationGeometryInstanceFlagsBits::TriangleCullDisable)
            res.flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        std::memcpy(dest, &res, sizeof(vk::AccelerationStructureInstanceKHR));
    }
    return sizeof(vk::AccelerationStructureInstanceKHR);
}

RHIDeviceScopedHandle<RHIAccelerationStructure>
VulkanDevice::CreateAccelerationStructure(RHIAccelerationStructureDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanAccelerationStructure>(*this, desc)};
}

RHIAccelerationStructure* VulkanDevice::GetAccelerationStructure(Handle handle) const
{
    return mStorage.GetObjectPtr<RHIAccelerationStructure>(handle);
}

void VulkanDevice::DestroyAccelerationStructure(Handle handle) { mStorage.DestroyObject(handle); }

void VulkanDeviceDescriptorSetLayout::DebugSetObjectName(const char* name)
{
    VkDescriptorSetLayout handle = *mLayout;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eDescriptorSetLayout,
                                                      .objectHandle = reinterpret_cast<uint64_t>(handle),
                                                      .pObjectName = name});
}

VulkanDeviceDescriptorSetLayout::VulkanDeviceDescriptorSetLayout(const VulkanDevice& device,
                                                                 RHIDeviceDescriptorSetLayoutDesc const& desc) :
    RHIDeviceDescriptorSetLayout(device, desc), mDevice(device)
{
    StackArena arena{};
    AllocatorStack alloc(arena);
    vk::DescriptorBindingFlags bindingFlags{};
    if (desc.updateAfterBind)
        bindingFlags |= vk::DescriptorBindingFlagBits::eUpdateAfterBind;
    Vector<vk::DescriptorSetLayoutBinding> bindings(desc.bindings.size(), alloc.Ptr());
    Vector<vk::DescriptorBindingFlags> flags(desc.bindings.size(), bindingFlags, alloc.Ptr());
    vk::DescriptorSetLayoutBindingFlagsCreateInfo bindInfo{.bindingCount = static_cast<uint32_t>(desc.bindings.size()),
                                                           .pBindingFlags = flags.data()};
    for (size_t i = 0; i < desc.bindings.size(); ++i)
    {
        auto const& b = desc.bindings[i];
        bindings[i] = vk::DescriptorSetLayoutBinding{
            .binding = static_cast<uint32_t>(i),
            .descriptorType = vkDescriptorTypeFromRHIDescriptorType(b.type),
            .descriptorCount = b.count,
            .stageFlags = vkShaderStageFlagsFromRHIShaderStage(b.stage),
        };
    }
    mLayout = vk::raii::DescriptorSetLayout(
        mDevice.GetVkDevice(),
        vk::DescriptorSetLayoutCreateInfo{.pNext = &bindInfo,
                                          .flags = desc.updateAfterBind
                                          ? vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool
                                          : vk::DescriptorSetLayoutCreateFlagBits{},
                                          .bindingCount = static_cast<uint32_t>(bindings.size()),
                                          .pBindings = bindings.data()},
        mDevice.GetVkAllocationCallbacks());
    CHECK_MSG(mLayout != nullptr, "failed to create Vulkan descriptor set layout");
}

RHIDeviceScopedHandle<RHIDeviceDescriptorSetLayout>
VulkanDevice::CreateDescriptorSetLayout(RHIDeviceDescriptorSetLayoutDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanDeviceDescriptorSetLayout>(*this, desc)};
}

RHIDeviceDescriptorSetLayout* VulkanDevice::GetDescriptorSetLayout(Handle handle) const
{
    return mStorage.GetObjectPtr<RHIDeviceDescriptorSetLayout>(handle);
}

void VulkanDevice::DestroyDescriptorSetLayout(Handle handle) { mStorage.DestroyObject(handle); }

#include "Descriptor.hpp"

RHIDeviceScopedHandle<RHIDeviceDescriptorPool>
VulkanDevice::CreateDescriptorPool(RHIDeviceDescriptorPool::PoolDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanDeviceDescriptorPool>(*this, desc)};
}

RHIDeviceDescriptorPool* VulkanDevice::GetDescriptorPool(Handle handle) const
{
    return mStorage.GetObjectPtr<RHIDeviceDescriptorPool>(handle);
}

void VulkanDevice::DestroyDescriptorPool(Handle handle) { mStorage.DestroyObject(handle); }


void VulkanDeviceSampler::DebugSetObjectName(const char* name)
{
    VkSampler handle = *mSampler;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eSampler,
                                                      .objectHandle = reinterpret_cast<uint64_t>(handle),
                                                      .pObjectName = name});
}

VulkanDeviceQueryPool::VulkanDeviceQueryPool(const VulkanDevice& device, QueryPoolDesc const& desc) :
    RHIDeviceQueryPool(device, desc), mDevice(device),
    mTimestampResolution(mDevice.GetVkPhysicalDevice().getProperties().limits.timestampPeriod),
    mResults(device.GetAllocator())
{
    vk::QueryPoolCreateInfo createInfo = {.queryCount = desc.count};
    switch (desc.type)
    {
    case QueryPoolDesc::Timestamp:
        createInfo.queryType = vk::QueryType::eTimestamp;
        break;
    case QueryPoolDesc::AccelerationStructureCompactedSize:
        createInfo.queryType = vk::QueryType::eAccelerationStructureCompactedSizeKHR;
        break;
    }
    mResults.resize(desc.count);
    mQueryPool = vk::raii::QueryPool(mDevice.GetVkDevice(), createInfo, mDevice.GetVkAllocationCallbacks());
    CHECK_MSG(mQueryPool != nullptr, "failed to create Vulkan query pool");
}

void VulkanDeviceQueryPool::Reset() { mQueryPool.reset(0, mDesc.count); }

Span<const uint64_t> VulkanDeviceQueryPool::GetResults(bool wait)
{
    VkResult res = vkGetQueryPoolResults(
        *mDevice.GetVkDevice(), *mQueryPool, 0, mDesc.count, sizeof(uint64_t) * mDesc.count, mResults.data(),
        sizeof(uint64_t), wait ? VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_64_BIT : VK_QUERY_RESULT_64_BIT);
    if (res == VK_NOT_READY)
        return {};
    CHECK(res == VK_SUCCESS);
    return mResults;
}

void VulkanDeviceQueryPool::DebugSetObjectName(const char* name)
{
    VkQueryPool handle = *mQueryPool;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eQueryPool,
                                                      .objectHandle = reinterpret_cast<uint64_t>(handle),
                                                      .pObjectName = name});
}

VulkanDeviceSampler::VulkanDeviceSampler(const VulkanDevice& device, SamplerDesc const& desc) :
    RHIDeviceSampler(device, desc), mDevice(device)
{
    auto vkFilterFromRHISamplerFilter = [](SamplerDesc::Filter::Type filter) -> vk::Filter
    {
        using enum SamplerDesc::Filter::Type;
        switch (filter)
        {
        case NearestNeighbor:
            return vk::Filter::eNearest;
        case Linear:
            return vk::Filter::eLinear;
        case Cubic:
            return vk::Filter::eCubicEXT; // TODO: check if supported
        default:
            throw std::runtime_error("unsupported sampler filter");
        }
    };
    auto vkSamplerAddressModeFromRHIAddressMode = [](SamplerDesc::AddressMode::Mode mode) -> vk::SamplerAddressMode
    {
        switch (mode)
        {
        case SamplerDesc::AddressMode::Repeat:
            return vk::SamplerAddressMode::eRepeat;
        case SamplerDesc::AddressMode::MirroredRepeat:
            return vk::SamplerAddressMode::eMirroredRepeat;
        case SamplerDesc::AddressMode::ClampToEdge:
            return vk::SamplerAddressMode::eClampToEdge;
        case SamplerDesc::AddressMode::ClampToBorder:
            return vk::SamplerAddressMode::eClampToBorder;
        case SamplerDesc::AddressMode::MirrorClampToEdge:
            return vk::SamplerAddressMode::eMirrorClampToEdge;
        default:
            throw std::runtime_error("unsupported sampler address mode");
        }
    };
    auto vkSamplerReductionModeFromRHIReductionMode = [](SamplerDesc::Reduction mode) -> vk::SamplerReductionMode
    {
        using enum SamplerDesc::Reduction;
        switch (mode)
        {
        case WeightedAverage:
            return vk::SamplerReductionMode::eWeightedAverage;
        case Min:
            return vk::SamplerReductionMode::eMin;
        case Max:
            return vk::SamplerReductionMode::eMax;
        default:
            throw std::runtime_error("unsupported sampler reduction mode");
        }
    };
    vk::SamplerReductionModeCreateInfo reduction{.reductionMode =
        vkSamplerReductionModeFromRHIReductionMode(desc.reduction)};
    vk::SamplerCreateInfo sampler{.magFilter = vkFilterFromRHISamplerFilter(desc.filter.magFilter),
                                  .minFilter = vkFilterFromRHISamplerFilter(desc.filter.minFilter),
                                  .mipmapMode = desc.mipmap.mipmapMode == SamplerDesc::Mipmap::Linear
                                  ? vk::SamplerMipmapMode::eLinear
                                  : vk::SamplerMipmapMode::eNearest,
                                  .addressModeU = vkSamplerAddressModeFromRHIAddressMode(desc.addressMode.u),
                                  .addressModeV = vkSamplerAddressModeFromRHIAddressMode(desc.addressMode.v),
                                  .addressModeW = vkSamplerAddressModeFromRHIAddressMode(desc.addressMode.w),
                                  .mipLodBias = desc.mipmap.bias,
                                  .anisotropyEnable = desc.anisotropy.enable,
                                  .maxAnisotropy = desc.anisotropy.maxLevel,
                                  .minLod = desc.lod.min,
                                  .maxLod = desc.lod.max};
    sampler.setPNext(&reduction);
    mSampler = vk::raii::Sampler(mDevice.GetVkDevice(), sampler, mDevice.GetVkAllocationCallbacks());
    CHECK_MSG(mSampler != nullptr, "failed to create Vulkan sampler");
}

RHIDeviceScopedHandle<RHIDeviceSampler> VulkanDevice::CreateSampler(RHIDeviceSampler::SamplerDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanDeviceSampler>(*this, desc)};
}

RHIDeviceSampler* VulkanDevice::GetSampler(Handle handle) const
{
    return mStorage.GetObjectPtr<RHIDeviceSampler>(handle);
}

void VulkanDevice::DestroySampler(Handle handle) { mStorage.DestroyObject(handle); }

RHIDeviceScopedHandle<RHIDeviceQueryPool> VulkanDevice::CreateQueryPool(RHIDeviceQueryPool::QueryPoolDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanDeviceQueryPool>(*this, desc)};
}

RHIDeviceQueryPool* VulkanDevice::GetQueryPool(Handle handle) const
{
    return mStorage.GetObjectPtr<RHIDeviceQueryPool>(handle);
}

void VulkanDevice::DestroyQueryPool(Handle handle) { mStorage.DestroyObject(handle); }

VulkanVirtualAllocator::VulkanVirtualAllocator(const VulkanDevice& device, uint64_t size) :
    mDevice(device), mAllocations(device.GetAllocator()), mCapacity(size)
{
    VmaVirtualBlockCreateInfo info{};
    info.size = size;
    CHECK(vmaCreateVirtualBlock(&info, &mBlock) == VK_SUCCESS && "failed to create VMA virtual block");
}

VulkanVirtualAllocator::~VulkanVirtualAllocator()
{
    if (mBlock)
        vmaDestroyVirtualBlock(mBlock);
}

uint64_t VulkanVirtualAllocator::Allocate(uint64_t size, uint64_t alignment)
{
    VmaVirtualAllocationCreateInfo info{};
    info.size = size;
    info.alignment = alignment;
    VmaVirtualAllocation alloc{};
    VkDeviceSize offset = 0;
    if (vmaVirtualAllocate(mBlock, &info, &alloc, &offset) != VK_SUCCESS)
        return kInvalidOffset;
    mAllocations.emplace(static_cast<uint64_t>(offset), alloc);
    mPeakUsage = std::max(mPeakUsage, static_cast<uint64_t>(offset) + size);
    return static_cast<uint64_t>(offset);
}

void VulkanVirtualAllocator::Free(uint64_t offset)
{
    auto it = mAllocations.find(offset);
    CHECK_MSG(it != mAllocations.end(), "RHIVirtualAllocator::Free of untracked offset {}", offset);
    vmaVirtualFree(mBlock, it->second);
    mAllocations.erase(it);
}

void VulkanVirtualAllocator::Clear()
{
    vmaClearVirtualBlock(mBlock);
    mAllocations.clear();
    mPeakUsage = 0;
}

uint64_t VulkanVirtualAllocator::GetUsedBytes() const
{
    VmaStatistics stats{};
    vmaGetVirtualBlockStatistics(mBlock, &stats);
    return stats.allocationBytes;
}

RHIDeviceScopedHandle<RHIVirtualAllocator> VulkanDevice::CreateVirtualAllocator(uint64_t size)
{
    return {this, mStorage.CreateObject<VulkanVirtualAllocator>(*this, size)};
}

RHIVirtualAllocator* VulkanDevice::GetVirtualAllocator(Handle handle) const
{
    return mStorage.GetObjectPtr<RHIVirtualAllocator>(handle);
}

void VulkanDevice::DestroyVirtualAllocator(Handle handle) { mStorage.DestroyObject(handle); }