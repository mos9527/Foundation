#include <algorithm>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Bits/Format.hpp>
#include <Core/Core.hpp>
#include <utility>

#include "Application.hpp"
#include "Device.hpp"

using namespace Foundation::Core;
using namespace Foundation::RHI;
using namespace Foundation::Bits;
const char* kVulkanDeviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_16BIT_STORAGE_EXTENSION_NAME,VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME};

const char* kVulkanDeviceTypes[] = {"Other", "Integrated GPU", "Discrete GPU", "Virtual GPU", "CPU"};

Allocator* VulkanDevice::GetAllocator() const { return mApp.GetAllocator(); }
vk::AllocationCallbacks const& VulkanDevice::GetVkAllocatorCallbacks() const { return mApp.GetVkAllocatorCallbacks(); }

VulkanDevice::VulkanDevice(VulkanApplication const& app, vk::raii::PhysicalDevice physicalDevice,
                           Native::NativeWindow* window) :
    RHIDevice(app), mApp(app), mPhysicalDevice(std::move(physicalDevice)), mSwapchainFormats(GetAllocator()),
    mSwapchainPresentModes(GetAllocator()), mStorage(GetAllocator()), window(window)
{
    LOG_RUNTIME(VulkanDevice, info, "Instantiating Vulkan device"), DebugLogDeviceInfo();
    auto queues = mPhysicalDevice.getQueueFamilyProperties();
    // Find queues
    // Graphics, Compute, Transfer should be preferably mutually exclusive
    // NOTE: We never used dedicated transfer in the Renderer - offloading to compute is more than enough for such
    // tasks.
    auto find_first = [&](vk::QueueFlags flags, uint32_t skip1, uint32_t skip2) -> uint32_t
    {
        for (uint32_t i = 0; i < queues.size(); ++i)
            if ((queues[i].queueFlags & flags) == flags && i != skip1 && i != skip2)
                return i;
        // If all queues are used, return the first usable one
        for (uint32_t i = 0; i < queues.size(); ++i)
            if ((queues[i].queueFlags & flags) == flags)
                return i;
        return kInvalidQueueIndex;
    };
    uint32_t graphics = find_first(vk::QueueFlagBits::eGraphics, -1, -1);
    uint32_t compute = find_first(vk::QueueFlagBits::eCompute, graphics, -1);
    uint32_t transfer = find_first(vk::QueueFlagBits::eTransfer, graphics, compute);
    if (graphics == kInvalidQueueIndex || compute == kInvalidQueueIndex || transfer == kInvalidQueueIndex)
    {
        throw std::runtime_error("unable to find queues for graphics, compute, or transfer");
    }
    uint32_t present = kInvalidQueueIndex; // Will be set later if a window is provided
    if (window)
    {
        // Check for a present queue
        VkSurfaceKHR surface;
        // NOTE: Creating surfaces is platform-dependent w/ requisite extensions. GLFW does this.
        if (glfwCreateWindowSurface(*mApp.GetVkInstance(), static_cast<GLFWwindow*>(window->GetNative()), nullptr,
                                    &surface) != VK_SUCCESS)
            throw std::runtime_error("Failed to create Vulkan surface for window");
        mSurface = vk::raii::SurfaceKHR(mApp.GetVkInstance(), surface);
        // Having present and graphics queues as the same avoids copies and is typically the case
        // - https://github.com/KhronosGroup/Vulkan-Hpp/blob/main/RAII_Samples/05_InitSwapchain/05_InitSwapchain.cpp#L45
        // - https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/master/src/VulkanSample.cpp#L1850
        if (mPhysicalDevice.getSurfaceSupportKHR(graphics, *mSurface))
        {
            present = graphics;
        }
        else
        {
            LOG_RUNTIME(VulkanDevice, warn, "Device may have separate Graphics and Present queues!");
            for (size_t i = 0; i < queues.size(); ++i)
            {
                if (mPhysicalDevice.getSurfaceSupportKHR(static_cast<uint32_t>(i), *mSurface))
                {
                    present = static_cast<int>(i);
                    break;
                }
            }
        }
        if (present == kInvalidQueueIndex)
            throw std::runtime_error("failed to find a valid present queue");
    }
    // Create the device
    // Gather all unique queues we'd need
    Set<uint32_t> unique_queues({graphics, compute, transfer, present}, GetAllocator());
    Vector<vk::DeviceQueueCreateInfo> queue_info(GetAllocator());
    float priority = 1.0f;
    for (auto i : unique_queues)
    {
        if (i == kInvalidQueueIndex)
            continue;
        queue_info.emplace_back(vk::DeviceQueueCreateInfo{
            .queueFamilyIndex = i,
            .queueCount = 1,
            .pQueuePriorities = &priority // All queues have the same priority
        });
    }
    vk::StructureChain<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
                       vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features,
                       vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>
        featureChain = {
            {.features = {.samplerAnisotropy = true, .shaderInt16 = true}}, // vk::PhysicalDeviceFeatures2
            {.storageInputOutput16 = true, .shaderDrawParameters = true}, // vk::PhysicalDeviceVulkan11Features
            {.drawIndirectCount = true,
             .shaderFloat16 = true,
             .runtimeDescriptorArray = true,
             .timelineSemaphore = true}, // vk::PhysicalDeviceVulkan12Features
            {.synchronization2 = true, .dynamicRendering = true}, // vk::PhysicalDeviceVulkan13Features
            {.extendedDynamicState = true}, // vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT
        };
    vk::DeviceCreateInfo device_info{.pNext = &featureChain.get<vk::PhysicalDeviceFeatures2>(),
                                     .queueCreateInfoCount = static_cast<uint32_t>(queue_info.size()),
                                     .pQueueCreateInfos = queue_info.data(),
                                     .enabledLayerCount = 0,
                                     .enabledExtensionCount = std::size(kVulkanDeviceExtensions),
                                     .ppEnabledExtensionNames = kVulkanDeviceExtensions};
    mDevice = vk::raii::Device(mPhysicalDevice, device_info, GetVkAllocatorCallbacks());
    CHECK(mDevice != nullptr && "failed to create Vulkan device");
    // Allocate the queues
    mQueues = ConstructUnique<VulkanDeviceQueues>(GetAllocator(), GetAllocator());
    for (auto i : unique_queues)
    {
        auto handle = mQueues->storage.CreateObject<VulkanDeviceQueue>(*this, i);
        if (i == graphics)
            mQueues->graphics = handle;
        if (i == compute)
            mQueues->compute = handle;
        if (i == transfer)
            mQueues->transfer = handle;
        if (i == present)
            mQueues->present = handle;
    }
    // Initialize VMA
    const VmaAllocatorCreateInfo allocator_info{
        .physicalDevice = *mPhysicalDevice,
        .device = *mDevice,
        .pAllocationCallbacks = reinterpret_cast<const VkAllocationCallbacks*>(&GetVkAllocatorCallbacks()),
        .instance = *mApp.GetVkInstance(),
        .vulkanApiVersion = mApp.mVulkanApiVersion};
    CHECK(vmaCreateAllocator(&allocator_info, &mVkAllocator) == VK_SUCCESS &&
          "failed to create VMA for Vulkan device");
    DebugLogAllocatorInfo();
    if (mSurface != nullptr)
    {
        // Collect swapchain (surface) info
        auto formats = mPhysicalDevice.getSurfaceFormatsKHR(mSurface);
        for (auto& fmt : formats)
        {
            using enum RHIResourceFormat;
            switch (fmt.format)
            {
            case vk::Format::eR8G8B8A8Unorm:
                mSwapchainFormats.emplace_back(R8G8B8A8Unorm);
                break;
            case vk::Format::eR8G8B8A8Srgb:
                mSwapchainFormats.emplace_back(R8G8B8A8Srgb);
                break;
            case vk::Format::eB8G8R8A8Unorm:
                mSwapchainFormats.emplace_back(B8G8R8A8Unrom);
                break;
            case vk::Format::eB8G8R8A8Srgb:
                mSwapchainFormats.emplace_back(B8G8R8A8Srgb);
                break;
            default:
                // TODO: More formats? HDR?
                break;
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
}

void VulkanDevice::DebugLogDeviceInfo() const
{
    auto properties = mPhysicalDevice.getProperties();
    LOG_RUNTIME(VulkanDevice, info,
                "** Vulkan Device Info **\n"
                "    driverVersion: 0x{:X}\n"
                "    vendorID: 0x{:X}\n"
                "    deviceID: 0x{:X}\n"
                "    deviceType: {}\n"
                "    deviceName: {}\n"
                "    limits:\n"
                "        maxMemoryAllocationCount: {}\n"
                "        bufferImageGranularity: {}\n"
                "        nonCoherentAtomSize: {}",
                properties.driverVersion, properties.vendorID, properties.deviceID,
                kVulkanDeviceTypes[static_cast<int>(properties.deviceType)], properties.deviceName.data(),
                properties.limits.maxMemoryAllocationCount,
                formatHumanReadableSize(properties.limits.bufferImageGranularity),
                formatHumanReadableSize(properties.limits.nonCoherentAtomSize));
}

/// https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/master/src/VulkanSample.cpp#L1694
void VulkanDevice::DebugLogAllocatorInfo() const
{
    const VkPhysicalDeviceProperties* props;
    const VkPhysicalDeviceMemoryProperties* memProps;
    vmaGetPhysicalDeviceProperties(mVkAllocator, &props);
    vmaGetMemoryProperties(mVkAllocator, &memProps);

    const uint32_t heapCount = memProps->memoryHeapCount;

    uint32_t deviceLocalHeapCount = 0;
    uint32_t hostVisibleHeapCount = 0;
    uint32_t deviceLocalAndHostVisibleHeapCount = 0;
    VkDeviceSize deviceLocalHeapSumSize = 0;
    VkDeviceSize hostVisibleHeapSumSize = 0;
    VkDeviceSize deviceLocalAndHostVisibleHeapSumSize = 0;

    for (uint32_t heapIndex = 0; heapIndex < heapCount; ++heapIndex)
    {
        const VkMemoryHeap& heap = memProps->memoryHeaps[heapIndex];
        const bool isDeviceLocal = (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        bool isHostVisible = false;
        for (uint32_t typeIndex = 0; typeIndex < memProps->memoryTypeCount; ++typeIndex)
        {
            const VkMemoryType& type = memProps->memoryTypes[typeIndex];
            if (type.heapIndex == heapIndex && (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            {
                isHostVisible = true;
                break;
            }
        }
        if (isDeviceLocal)
        {
            ++deviceLocalHeapCount;
            deviceLocalHeapSumSize += heap.size;
        }
        if (isHostVisible)
        {
            ++hostVisibleHeapCount;
            hostVisibleHeapSumSize += heap.size;
            if (isDeviceLocal)
            {
                ++deviceLocalAndHostVisibleHeapCount;
                deviceLocalAndHostVisibleHeapSumSize += heap.size;
            }
        }
    }

    uint32_t hostVisibleNotHostCoherentTypeCount = 0;
    uint32_t notDeviceLocalNotHostVisibleTypeCount = 0;
    uint32_t amdSpecificTypeCount = 0;
    uint32_t lazilyAllocatedTypeCount = 0;
    uint32_t allTypeBits = 0;
    uint32_t deviceLocalTypeBits = 0;
    for (uint32_t typeIndex = 0; typeIndex < memProps->memoryTypeCount; ++typeIndex)
    {
        const VkMemoryType& type = memProps->memoryTypes[typeIndex];
        allTypeBits |= 1u << typeIndex;
        if (type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            deviceLocalTypeBits |= 1u << typeIndex;
        if ((type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
            ++hostVisibleNotHostCoherentTypeCount;
        if ((type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0 &&
            (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
            ++notDeviceLocalNotHostVisibleTypeCount;
        if (type.propertyFlags &
            (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD))
            ++amdSpecificTypeCount;
        if (type.propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)
            ++lazilyAllocatedTypeCount;
    }
    LOG_RUNTIME(VulkanDevice, info,
                "** Vulkan Allocator Stats **\n"
                "    deviceLocalHeapCount = {}\n"
                "    deviceLocalHeapSumSize = {}\n"
                "    hostVisibleHeapCount = {}\n"
                "    hostVisibleHeapSumSize = {}\n"
                "    deviceLocalAndHostVisibleHeapCount = {}\n"
                "    deviceLocalAndHostVisibleHeapSumSize = {}\n"
                "    hostVisibleNotHostCoherentTypeCount = {}\n"
                "    notDeviceLocalNotHostVisibleTypeCount = {}\n"
                "    amdSpecificTypeCount = {}\n"
                "    lazilyAllocatedTypeCount = {}",
                deviceLocalHeapCount, formatHumanReadableSize(deviceLocalHeapSumSize), hostVisibleHeapCount,
                formatHumanReadableSize(hostVisibleHeapSumSize), deviceLocalAndHostVisibleHeapCount,
                formatHumanReadableSize(deviceLocalAndHostVisibleHeapSumSize), hostVisibleNotHostCoherentTypeCount,
                notDeviceLocalNotHostVisibleTypeCount, amdSpecificTypeCount, lazilyAllocatedTypeCount);
}

VulkanDevice::~VulkanDevice()
{
    if (mVkAllocator)
    {
        vmaDestroyAllocator(mVkAllocator);
        mVkAllocator = nullptr;
    }
}

void VulkanDevice::WaitIdle() const { mDevice.waitIdle(); }

VulkanDeviceQueue* VulkanDeviceQueues::Get(Handle handle) const { return storage.GetObjectPtr(handle); }
RHIDeviceQueue* VulkanDevice::GetDeviceQueue(RHIDeviceQueueType type) const
{
    switch (type)
    {
    case RHIDeviceQueueType::Present:
        return mQueues->Get(mQueues->present);
    case RHIDeviceQueueType::Compute:
        return mQueues->Get(mQueues->compute);
    case RHIDeviceQueueType::Transfer:
        return mQueues->Get(mQueues->transfer);
    case RHIDeviceQueueType::Graphics:
        return mQueues->Get(mQueues->graphics);
    default:
        break;
    }
    return nullptr;
}

#include "Swapchain.hpp"
Span<RHIResourceFormat const> VulkanDevice::GetSwapchainSupportedFormats() const { return mSwapchainFormats; }
Span<RHISwapchainPresentMode const> VulkanDevice::GetSwapchainSupportedPresentModes() const
{
    return mSwapchainPresentModes;
}
RHIDeviceScopedObjectHandle<RHISwapchain> VulkanDevice::CreateSwapchain(RHISwapchain::SwapchainDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanSwapchain>(*this, desc)};
}
RHISwapchain* VulkanDevice::GetSwapchain(Handle handle) const { return mStorage.GetObjectPtr<RHISwapchain>(handle); };
void VulkanDevice::DestroySwapchain(Handle handle) { mStorage.DestroyObject(handle); }

#include "PipelineState.hpp"
RHIDeviceScopedObjectHandle<RHIPipelineState>
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
RHIDeviceScopedObjectHandle<RHIShaderModule>
VulkanDevice::CreateShaderModule(RHIShaderModule::ShaderModuleDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanShaderModule>(*this, desc)};
}
RHIShaderModule* VulkanDevice::GetShaderModule(Handle handle) const
{
    return mStorage.GetObjectPtr<RHIShaderModule>(handle);
}
void VulkanDevice::DestroyShaderModule(Handle handle) { mStorage.DestroyObject(handle); }

#include "Command.hpp"
RHIDeviceScopedObjectHandle<RHICommandPool> VulkanDevice::CreateCommandPool(RHICommandPool::PoolDesc desc)
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
    vk::SemaphoreTypeCreateInfo tinfo{
        .semaphoreType = vk::SemaphoreType::eTimeline,
        .initialValue = 0
    };
    if (is_timeline)
        info.setPNext(&tinfo);
    mSemaphore = vk::raii::Semaphore(mDevice.GetVkDevice(), info, device.GetVkAllocatorCallbacks());
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
        device.GetVkAllocatorCallbacks()))
{
}
void VulkanDeviceFence::DebugSetObjectName(const char* name)
{
    VkFence handle = *mFence;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eFence,
                                                       .objectHandle = reinterpret_cast<uint64_t>(handle),
                                                       .pObjectName = name});
}

RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> VulkanDevice::CreateSemaphore(bool is_timeline)
{
    return {this, mStorage.CreateObject<VulkanDeviceSemaphore>(*this, is_timeline)};
}
RHIDeviceSemaphore* VulkanDevice::GetSemaphore(Handle handle) const
{
    return mStorage.GetObjectPtr<RHIDeviceSemaphore>(handle);
}
void VulkanDevice::DestroySemaphore(Handle handle) { mStorage.DestroyObject(handle); }

auto VulkanDevice::CreateFence(bool signaled) -> RHIDeviceScopedObjectHandle<RHIDeviceFence>
{
    return {this, mStorage.CreateObject<VulkanDeviceFence>(*this, signaled)};
}
RHIDeviceFence* VulkanDevice::GetFence(Handle handle) const { return mStorage.GetObjectPtr<RHIDeviceFence>(handle); }
void VulkanDevice::DestroyFence(Handle handle) { mStorage.DestroyObject(handle); }

void VulkanDevice::ResetFences(Span<const RHIDeviceObjectHandle<RHIDeviceFence>> fences)
{
    StackArena<> arena;
    StackAllocator alloc(arena);
    Vector<vk::Fence> vk_fences(alloc.Ptr());
    vk_fences.reserve(fences.size());
    for (auto const& fence : fences)
        vk_fences.emplace_back(fence.Get<VulkanDeviceFence>()->GetVkFence());
    mDevice.resetFences(vk_fences);
}
void VulkanDevice::WaitForFences(Span<const RHIDeviceObjectHandle<RHIDeviceFence>> fences, bool wait_all,
                                 size_t timeout)
{
    StackArena<> arena;
    StackAllocator alloc(arena);
    Vector<vk::Fence> vk_fences(alloc.Ptr());
    vk_fences.reserve(fences.size());
    for (auto const& fence : fences)
        vk_fences.emplace_back(fence.Get<VulkanDeviceFence>()->GetVkFence());
    vk::Result res = mDevice.waitForFences(vk_fences, wait_all, timeout);
    CHECK_MSG(res == vk::Result::eSuccess, "Failed waiting on fences");
}

void VulkanDevice::SignalTimelineSemaphores(
    Span<const Pair<RHIDeviceObjectHandle<RHIDeviceSemaphore>, size_t>> semaphores)
{
    for (auto const& [signal, val] : semaphores)
    {
        CHECK(signal->mIsTimeline);
        vk::SemaphoreSignalInfo info{.semaphore = signal.Get<VulkanDeviceSemaphore>()->GetVkSemaphore(), .value = val};
        mDevice.signalSemaphore(info);
    }
}
void VulkanDevice::WaitForTimelineSemaphores(
    Span<const Pair<RHIDeviceObjectHandle<RHIDeviceSemaphore>, size_t>> semaphores, size_t timeout)
{
    StackArena<> arena{};
    StackAllocator alloc(arena);
    Vector<vk::Semaphore> vk_semaphores(alloc.Ptr());
    Vector<uint64_t> vk_values(alloc.Ptr());
    vk_semaphores.reserve(semaphores.size()), vk_values.reserve(semaphores.size());
    for (auto const& [wait, val] : semaphores)
    {
        CHECK(wait->mIsTimeline);
        vk_semaphores.emplace_back(wait.Get<VulkanDeviceSemaphore>()->GetVkSemaphore()), vk_values.emplace_back(val);
    }
    auto res =
        mDevice.waitSemaphores(vk::SemaphoreWaitInfo{.semaphoreCount = static_cast<uint32_t>(vk_semaphores.size()),
                                                      .pSemaphores = vk_semaphores.data(),
                                                      .pValues = vk_values.data()},
                                timeout);
    CHECK(res == vk::Result::eSuccess && "failed to wait for semaphores");
}

void VulkanDevice::DebugSetObjectName(const char* name)
{
    VkDevice handle = *mDevice;
    mDevice.setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eDevice,
                                         .objectHandle = reinterpret_cast<uint64_t>(handle),
                                         .pObjectName = name});
}

void VulkanDeviceQueue::WaitIdle() const { mQueue.waitIdle(); }
void VulkanDeviceQueue::Submit(SubmitDesc const& desc) const
{
    StackArena<4096> arena{};
    StackAllocator alloc(arena);
    Vector<vk::CommandBuffer> cmds(alloc.Ptr());
    Vector<vk::Semaphore> swaits(alloc.Ptr()), ssignals(alloc.Ptr());
    Vector<uint64_t> wait_values(alloc.Ptr()), signal_values(alloc.Ptr());
    cmds.reserve(desc.cmdLists.size()), swaits.reserve(desc.waits.size()), ssignals.reserve(desc.signals.size());
    wait_values.reserve(desc.timelineWaits.size()), signal_values.reserve(desc.timelineSignals.size());
    for (auto const& cmd_list : desc.cmdLists)
        cmds.emplace_back(static_cast<VulkanCommandList*>(cmd_list)->GetVkCommandBuffer());

    for (auto const& [wait, val] : desc.timelineWaits)
    {
        CHECK(wait->mIsTimeline && "Submit() timeline_signals must be Timeline semaphores");
        swaits.emplace_back(static_cast<VulkanDeviceSemaphore*>(wait)->GetVkSemaphore());
        wait_values.emplace_back(val);
    }
    for (auto const& [signal, val] : desc.timelineSignals)
    {
        CHECK(signal->mIsTimeline && "Submit() timeline_signals must be Timeline semaphores");
        ssignals.emplace_back(static_cast<VulkanDeviceSemaphore*>(signal)->GetVkSemaphore());
        signal_values.emplace_back(val);
    }
    for (auto const& wait : desc.waits)
    {
        CHECK(!wait->mIsTimeline && "Submit() waits must be Binary semaphores");
        swaits.emplace_back(static_cast<VulkanDeviceSemaphore*>(wait)->GetVkSemaphore());
        // https://registry.khronos.org/vulkan/specs/latest/man/html/VkTimelineSemaphoreSubmitInfo.html#_description
        // Driver should ignore these
        wait_values.emplace_back(0);
    }
    for (auto const& signal : desc.signals)
    {
        CHECK(!signal->mIsTimeline && "Submit() signals must be Binary semaphores");
        ssignals.emplace_back(static_cast<VulkanDeviceSemaphore*>(signal)->GetVkSemaphore());
        signal_values.emplace_back(0);
    }
    CHECK_MSG(desc.waitsStages.size() == swaits.size(),
              "Number of wait stages ({}) must match number of wait semaphores ({})", desc.waitsStages.size(),
              swaits.size());
    Vector<vk::PipelineStageFlags> stages(alloc.Ptr());
    stages.reserve(desc.waitsStages.size());
    for (auto stage : desc.waitsStages)
        stages.emplace_back(vkPipelineStageFlagsFromRHIPipelineStage(stage));
    vk::SubmitInfo info{.waitSemaphoreCount = static_cast<uint32_t>(swaits.size()),
                        .pWaitSemaphores = swaits.data(),
                        .pWaitDstStageMask = stages.data(),
                        .commandBufferCount = static_cast<uint32_t>(cmds.size()),
                        .pCommandBuffers = cmds.data(),
                        .signalSemaphoreCount = static_cast<uint32_t>(ssignals.size()),
                        .pSignalSemaphores = ssignals.data()};
    vk::TimelineSemaphoreSubmitInfo tinfo{.waitSemaphoreValueCount = static_cast<uint32_t>(wait_values.size()),
                                          .pWaitSemaphoreValues = wait_values.data(),
                                          .signalSemaphoreValueCount = static_cast<uint32_t>(signal_values.size()),
                                          .pSignalSemaphoreValues = signal_values.data()};
    if (!wait_values.empty() || !signal_values.empty())
        info.setPNext(&tinfo);
    {
        mQueue.submit(info, desc.fence ? static_cast<VulkanDeviceFence*>(desc.fence)->GetVkFence() : vk::Fence(nullptr));
    }
}
void VulkanDeviceQueue::Present(PresentDesc const& desc) const
{
    CHECK(mDevice.GetDeviceQueue(RHIDeviceQueueType::Present) == this &&
          "Present called on a queue that is not a present queue");
    StackArena<4096> arena{};
    StackAllocator alloc(arena);
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

RHIDeviceScopedObjectHandle<RHIBuffer> VulkanDevice::CreateBuffer(RHIBufferDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanBuffer>(*this, desc)};
}
RHIBuffer* VulkanDevice::GetBuffer(Handle handle) const { return mStorage.GetObjectPtr<RHIBuffer>(handle); }
void VulkanDevice::DestroyBuffer(Handle handle) { mStorage.DestroyObject(handle); }

RHIDeviceScopedObjectHandle<RHITexture> VulkanDevice::CreateTexture(RHITextureDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanTexture>(*this, desc)};
}
RHITexture* VulkanDevice::GetImage(Handle handle) const { return mStorage.GetObjectPtr<RHITexture>(handle); }
void VulkanDevice::DestroyImage(Handle handle) { mStorage.DestroyObject(handle); }

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
    StackArena<> arena{};
    StackAllocator alloc(arena);
    Vector<vk::DescriptorSetLayoutBinding> bindings(desc.bindings.size(), alloc.Ptr());
    for (size_t i = 0; i < desc.bindings.size(); ++i)
    {
        auto const& b = desc.bindings[i];
        bindings[i] = vk::DescriptorSetLayoutBinding{.binding = static_cast<uint32_t>(i),
                                                     .descriptorType = vkDescriptorTypeFromRHIDescriptorType(b.type),
                                                     .descriptorCount = b.count,
                                                     .stageFlags = vkShaderStageFlagsFromRHIShaderStage(b.stage)};
    }
    mLayout = vk::raii::DescriptorSetLayout(
        mDevice.GetVkDevice(),
        vk::DescriptorSetLayoutCreateInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                          .pBindings = bindings.data()},
        mDevice.GetVkAllocatorCallbacks());
    CHECK_MSG(mLayout != nullptr, "failed to create Vulkan descriptor set layout");
}
RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout>
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
RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool>
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
    mSampler = vk::raii::Sampler(
        mDevice.GetVkDevice(),
        vk::SamplerCreateInfo{.magFilter = vkFilterFromRHISamplerFilter(desc.filter.magFilter),
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
                              .maxLod = desc.lod.max},
        mDevice.GetVkAllocatorCallbacks());
    CHECK_MSG(mSampler != nullptr, "failed to create Vulkan sampler");
}
RHIDeviceScopedObjectHandle<RHIDeviceSampler> VulkanDevice::CreateSampler(RHIDeviceSampler::SamplerDesc const& desc)
{
    return {this, mStorage.CreateObject<VulkanDeviceSampler>(*this, desc)};
}
RHIDeviceSampler* VulkanDevice::GetSampler(Handle handle) const
{
    return mStorage.GetObjectPtr<RHIDeviceSampler>(handle);
}
void VulkanDevice::DestroySampler(Handle handle) { mStorage.DestroyObject(handle); }
