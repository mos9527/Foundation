#include <algorithm>

#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Bits/Format.hpp>
#include <Core/Core.hpp>

#include "Application.hpp"
#include "Device.hpp"
using namespace Foundation::Core;
using namespace Foundation::RHI;

const char* kVulkanDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

const char* kVulkanDeviceTypes[] = {
    "Other",
    "Integrated GPU",
    "Discrete GPU",
    "Virtual GPU",
    "CPU"
};

Allocator* VulkanDevice::GetAllocator() const {
    return m_app.GetAllocator();
}
vk::AllocationCallbacks const& VulkanDevice::GetVkAllocatorCallbacks() const {
    return m_app.GetVkAllocatorCallbacks();
}

VulkanDevice::VulkanDevice(VulkanApplication const& app, const vk::raii::PhysicalDevice& physicalDevice, Native::NativeWindow* window) :
    m_app(app), m_physicalDevice(physicalDevice), RHIDevice(app), m_storage(GetAllocator(), kDeviceStorageReserveSize) {
    LOG_RUNTIME(VulkanDevice, info, "Instantiating Vulkan device"), DebugLogDeviceInfo();
    auto queues = m_physicalDevice.getQueueFamilyProperties();
    // Find queues
    // Graphics, Compute, Transfer should be preferably mutually exclusive
    auto find_first = [&](vk::QueueFlags flags, uint32_t skip1, uint32_t skip2) -> uint32_t {
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
    if (graphics == kInvalidQueueIndex || compute == kInvalidQueueIndex || transfer == kInvalidQueueIndex) {
        throw std::runtime_error("unable to find queues for graphics, compute, or transfer");
    }
    uint32_t present = kInvalidQueueIndex; // Will be set later if a window is provided
    if (window) {
        // Check for a present queue
        VkSurfaceKHR surface;
        // NOTE: Creating surfaces is platform-dependent w/ requisite extensions. GLFW does this.
        if (glfwCreateWindowSurface(*m_app.GetVkInstance(), (GLFWwindow*)window->GetNative(), nullptr, &surface) != VK_SUCCESS)
            throw std::runtime_error("Failed to create Vulkan surface for window");
        m_surface = vk::raii::SurfaceKHR(m_app.GetVkInstance(), surface);
        // Having present and graphics queues as the same avoids copies and is typically the case       
        // - https://github.com/KhronosGroup/Vulkan-Hpp/blob/main/RAII_Samples/05_InitSwapchain/05_InitSwapchain.cpp#L45
        // - https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/master/src/VulkanSample.cpp#L1850
        if (m_physicalDevice.getSurfaceSupportKHR(graphics, *m_surface)) {
            present = graphics;
        }
        else {
            for (size_t i = 0; i < queues.size(); ++i) {
                if (m_physicalDevice.getSurfaceSupportKHR(static_cast<uint32_t>(i), *m_surface)) {
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
    Core::StlSet<uint32_t> unique_queues({ graphics, compute, transfer, present }, GetAllocator());
    Core::StlVector<vk::DeviceQueueCreateInfo> queue_info(GetAllocator());
    float priority = 1.0f;
    for (auto i : unique_queues) {
        if (i == kInvalidQueueIndex)
            continue;
        queue_info.emplace_back(vk::DeviceQueueCreateInfo{
            .queueFamilyIndex = i,
            .queueCount = 1,
            .pQueuePriorities = &priority // All queues have the same priority
            });
    }
    vk::StructureChain<
        vk::PhysicalDeviceFeatures2,
        vk::PhysicalDeviceVulkan12Features,
        vk::PhysicalDeviceVulkan13Features,
        vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT       
    > featureChain = {
        {.features = {.samplerAnisotropy = true } },            // vk::PhysicalDeviceFeatures2
        {.shaderFloat16 = true, .timelineSemaphore = true },    // vk::PhysicalDeviceVulkan12Features
        {.synchronization2 = true, .dynamicRendering = true },  // vk::PhysicalDeviceVulkan13Features
        {.extendedDynamicState = true }                         // vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT        
    };
    vk::DeviceCreateInfo device_info{
            .pNext = &featureChain.get<vk::PhysicalDeviceFeatures2>(),
            .queueCreateInfoCount = static_cast<uint32_t>(queue_info.size()),
            .pQueueCreateInfos = queue_info.data(),
            .enabledLayerCount = 0,
            .enabledExtensionCount = sizeof(kVulkanDeviceExtensions) / sizeof(kVulkanDeviceExtensions[0]),
            .ppEnabledExtensionNames = kVulkanDeviceExtensions
    };
    m_device = vk::raii::Device(m_physicalDevice, device_info, GetVkAllocatorCallbacks());
    CHECK(m_device != nullptr && "failed to create Vulkan device");
    // Allocate the queues
    m_queues = Core::ConstructUnique<VulkanDeviceQueues>(GetAllocator(), GetAllocator());
    for (auto i : unique_queues) {
        auto handle = m_queues->storage.CreateObject<VulkanDeviceQueue>(*this, i);
        if (i == graphics) m_queues->graphics = handle;
        if (i == compute) m_queues->compute = handle;
        if (i == transfer) m_queues->transfer = handle;
        if (i == present) m_queues->present = handle;
    }
    // Initialize VMA
    const VmaAllocatorCreateInfo allocator_info{
        .physicalDevice = *m_physicalDevice,
        .device = *m_device,
        .pAllocationCallbacks = reinterpret_cast<const VkAllocationCallbacks*>(&GetVkAllocatorCallbacks()),
        .instance = *m_app.GetVkInstance(),
        .vulkanApiVersion = m_app.m_vulkanApiVersion
    };
    CHECK(vmaCreateAllocator(&allocator_info, &m_vkAllocator) == VK_SUCCESS && "failed to create VMA for Vulkan device");
    DebugLogAllocatorInfo();
}

void VulkanDevice::DebugLogDeviceInfo() const {
    auto properties = m_physicalDevice.getProperties();
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
        properties.driverVersion,
        properties.vendorID,
        properties.deviceID,
        kVulkanDeviceTypes[static_cast<int>(properties.deviceType)],
        properties.deviceName.data(),
        properties.limits.maxMemoryAllocationCount,
        formatHumanReadableSize(properties.limits.bufferImageGranularity),
        formatHumanReadableSize(properties.limits.nonCoherentAtomSize)
    );
}

/// https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/master/src/VulkanSample.cpp#L1694
void VulkanDevice::DebugLogAllocatorInfo() const {
    const VkPhysicalDeviceProperties* props;
    const VkPhysicalDeviceMemoryProperties* memProps;
    vmaGetPhysicalDeviceProperties(m_vkAllocator, &props);
    vmaGetMemoryProperties(m_vkAllocator, &memProps);

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
        if (type.propertyFlags & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD))
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
        deviceLocalHeapCount,
        formatHumanReadableSize(deviceLocalHeapSumSize),
        hostVisibleHeapCount,
        formatHumanReadableSize(hostVisibleHeapSumSize),
        deviceLocalAndHostVisibleHeapCount,
        formatHumanReadableSize(deviceLocalAndHostVisibleHeapSumSize),
        hostVisibleNotHostCoherentTypeCount,
        notDeviceLocalNotHostVisibleTypeCount,
        amdSpecificTypeCount,
        lazilyAllocatedTypeCount
    );
}

VulkanDevice::~VulkanDevice() {    
    if (m_vkAllocator) {
        vmaDestroyAllocator(m_vkAllocator);
        m_vkAllocator = nullptr;
    }
}

void VulkanDevice::WaitIdle() const {
    m_device.waitIdle();
}

VulkanDeviceQueue* VulkanDeviceQueues::Get(Handle handle) const { return storage.GetObjectPtr(handle); }
RHIDeviceQueue* VulkanDevice::GetDeviceQueue(RHIDeviceQueueType type) const {
    switch (type) {
        case RHIDeviceQueueType::Present:  return m_queues->Get(m_queues->present);        
        case RHIDeviceQueueType::Compute:  return m_queues->Get(m_queues->compute);
        case RHIDeviceQueueType::Transfer: return m_queues->Get(m_queues->transfer);
        case RHIDeviceQueueType::Graphics: return m_queues->Get(m_queues->graphics);
        default:break;
    }
    return nullptr;
}

#include "Swapchain.hpp"
RHIDeviceScopedObjectHandle<RHISwapchain> VulkanDevice::CreateSwapchain(RHISwapchain::SwapchainDesc const& desc) {
    return { this, m_storage.CreateObject<VulkanSwapchain>(*this, desc) };
}
RHISwapchain* VulkanDevice::GetSwapchain(Handle handle) const {
    return m_storage.GetObjectPtr<RHISwapchain>(handle);
};
void VulkanDevice::DestroySwapchain(Handle handle) {
    m_storage.DestroyObject(handle);
}

#include "PipelineState.hpp"
RHIDeviceScopedObjectHandle<RHIPipelineState> VulkanDevice::CreatePipelineState(RHIPipelineState::PipelineStateDesc const& desc) {
    return { this, m_storage.CreateObject<VulkanPipelineState>(*this, desc) };
}
RHIPipelineState* VulkanDevice::GetPipelineState(Handle handle) const {
    return m_storage.GetObjectPtr<RHIPipelineState>(handle);
}
void VulkanDevice::DestroyPipelineState(Handle handle) {
    m_storage.DestroyObject(handle);
}

#include "Shader.hpp"
RHIDeviceScopedObjectHandle<RHIShaderModule> VulkanDevice::CreateShaderModule(RHIShaderModule::ShaderModuleDesc const& desc) {
    return { this, m_storage.CreateObject<VulkanShaderModule>(*this, desc) };
}
RHIShaderModule* VulkanDevice::GetShaderModule(Handle handle) const {
    return m_storage.GetObjectPtr<RHIShaderModule>(handle);
}
void VulkanDevice::DestroyShaderModule(Handle handle) {
    m_storage.DestroyObject(handle);
}

#include "Command.hpp"
RHIDeviceScopedObjectHandle<RHICommandPool> VulkanDevice::CreateCommandPool(RHICommandPool::PoolDesc desc)
{
    return { this, m_storage.CreateObject<VulkanCommandPool>(*this, desc, GetAllocator()) };
}
RHICommandPool* VulkanDevice::GetCommandPool(Handle handle) const {
    return m_storage.GetObjectPtr<RHICommandPool>(handle);
}
void VulkanDevice::DestroyCommandPool(Handle handle) {
    m_storage.DestroyObject(handle);
}

VulkanDeviceSemaphore::VulkanDeviceSemaphore(const VulkanDevice& device, bool is_timeline)
    : RHIDeviceSemaphore(device, is_timeline), m_device(device) {
    vk::SemaphoreCreateInfo info{};
    if (is_timeline) {
        vk::SemaphoreTypeCreateInfo tinfo{};
        tinfo.semaphoreType = vk::SemaphoreType::eTimeline;
        tinfo.initialValue = 0;
        info.setPNext(&tinfo);
    }
    m_semaphore = vk::raii::Semaphore(m_device.GetVkDevice(), info, device.GetVkAllocatorCallbacks());
}
void VulkanDeviceSemaphore::DebugSetObjectName(const char* name) {
    VkSemaphore handle = *m_semaphore;
    m_device.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eSemaphore,
        .objectHandle = (uint64_t)(handle),
        .pObjectName = name
    });
}
VulkanDeviceFence::VulkanDeviceFence(const VulkanDevice& device, bool signaled)
    : RHIDeviceFence(device), m_device(device),
    m_fence(vk::raii::Fence(device.GetVkDevice(), vk::FenceCreateInfo{
        .flags = signaled ? vk::FenceCreateFlagBits::eSignaled : vk::FenceCreateFlags{}
    }, device.GetVkAllocatorCallbacks())) {}
void VulkanDeviceFence::DebugSetObjectName(const char* name) {
    VkFence handle = *m_fence;
    m_device.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eFence,
        .objectHandle = (uint64_t)(handle),
        .pObjectName = name
        });
}

RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> VulkanDevice::CreateSemaphore(bool is_timeline)
{
    return { this, m_storage.CreateObject<VulkanDeviceSemaphore>(*this, is_timeline) };
}
RHIDeviceSemaphore* VulkanDevice::GetSemaphore(Handle handle) const
{
    return m_storage.GetObjectPtr<RHIDeviceSemaphore>(handle);
}
void VulkanDevice::DestroySemaphore(Handle handle)
{
    m_storage.DestroyObject(handle);
}

RHIDeviceScopedObjectHandle<RHIDeviceFence> VulkanDevice::CreateFence(bool signaled)
{
    return { this, m_storage.CreateObject<VulkanDeviceFence>(*this, signaled) };
}
RHIDeviceFence* VulkanDevice::GetFence(Handle handle) const
{
    return m_storage.GetObjectPtr<RHIDeviceFence>(handle);
}
void VulkanDevice::DestroyFence(Handle handle)
{
    m_storage.DestroyObject(handle);
}

void VulkanDevice::ResetFences(Core::StlSpan<const RHIDeviceObjectHandle<RHIDeviceFence>> fences)
{
    Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
    Core::StlVector<vk::Fence> vk_fences(alloc.Ptr());
    vk_fences.reserve(fences.size());
    for (auto const& fence : fences)
        vk_fences.emplace_back(fence.Get<VulkanDeviceFence>()->GetVkFence());
    m_device.resetFences(vk_fences);
}
void VulkanDevice::WaitForFences(Core::StlSpan<const RHIDeviceObjectHandle<RHIDeviceFence>> fences, bool wait_all, size_t timeout)
{
    Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
    Core::StlVector<vk::Fence> vk_fences(alloc.Ptr());
    vk_fences.reserve(fences.size());
    for (auto const& fence : fences)
        vk_fences.emplace_back(fence.Get<VulkanDeviceFence>()->GetVkFence());
    auto res = m_device.waitForFences(vk_fences, wait_all, timeout);
    // !! TODO
}

void VulkanDevice::SignalTimelineSemaphores(Core::StlSpan<const std::pair<RHIDeviceObjectHandle<RHIDeviceSemaphore>, size_t>> semaphores) {
    for (auto const& [signal, val] : semaphores) {
        CHECK(signal->m_is_timeline);
        vk::SemaphoreSignalInfo info{
            .semaphore = signal.Get<VulkanDeviceSemaphore>()->GetVkSemaphore(),
            .value = val
        };
        m_device.signalSemaphore(info);
    }
}
void VulkanDevice::WaitForTimelineSemaphores(Core::StlSpan<const std::pair<RHIDeviceObjectHandle<RHIDeviceSemaphore>, size_t>> semaphores, size_t timeout) {
    Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
    Core::StlVector<vk::Semaphore> vk_semaphores(alloc.Ptr());
    Core::StlVector<uint64_t> vk_values(alloc.Ptr());
    vk_semaphores.reserve(semaphores.size()), vk_values.reserve(semaphores.size());
    for (auto const& [wait, val] : semaphores) {
        CHECK(wait->m_is_timeline);
        vk_semaphores.emplace_back(wait.Get<VulkanDeviceSemaphore>()->GetVkSemaphore()),
            vk_values.emplace_back(val);
    }
    auto res = m_device.waitSemaphores(vk::SemaphoreWaitInfo{
        .semaphoreCount = static_cast<uint32_t>(vk_semaphores.size()),
        .pSemaphores = vk_semaphores.data(),
        .pValues = vk_values.data()
    }, timeout);
    CHECK(res == vk::Result::eSuccess && "failed to wait for semaphores");
}

void VulkanDevice::DebugSetObjectName(const char* name) {
    VkDevice handle = *m_device;
    m_device.setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eDevice,
        .objectHandle = (uint64_t)(handle),
        .pObjectName = name
        });
}

void VulkanDeviceQueue::WaitIdle() const {
    m_queue.waitIdle();
}
void VulkanDeviceQueue::Submit(SubmitDesc const& desc) const {
    Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
    Core::StlVector<vk::CommandBuffer> cmds(alloc.Ptr());
    Core::StlVector<vk::Semaphore>
        swaits(alloc.Ptr()), ssignals(alloc.Ptr());
    Core::StlVector<uint64_t>
        wait_values(alloc.Ptr()), signal_values(alloc.Ptr());
    cmds.reserve(desc.cmd_lists.size()), swaits.reserve(desc.waits.size()), ssignals.reserve(desc.signals.size());
    for (auto const& cmd_list : desc.cmd_lists)
        cmds.emplace_back(static_cast<VulkanCommandList*>(cmd_list)->GetVkCommandBuffer());
    for (auto const& [wait, val] : desc.timeline_waits) {
        CHECK(wait->m_is_timeline && "Submit() timeline_signals must be Timeline semaphores");
        swaits.emplace_back(static_cast<VulkanDeviceSemaphore*>(wait)->GetVkSemaphore());
        wait_values.emplace_back(val);
    }
    for (auto const& [signal, val] : desc.timeline_signals) {
        CHECK(signal->m_is_timeline && "Submit() timeline_signals must be Timeline semaphores");
        ssignals.emplace_back(static_cast<VulkanDeviceSemaphore*>(signal)->GetVkSemaphore());
        signal_values.emplace_back(val);
    }
    for (auto const& wait : desc.waits) {
        CHECK(!wait->m_is_timeline && "Submit() waits must be Binary semaphores");
        swaits.emplace_back(static_cast<VulkanDeviceSemaphore*>(wait)->GetVkSemaphore());
    }
    for (auto const& signal : desc.signals) {
        CHECK(!signal->m_is_timeline && "Submit() signals must be Binary semaphores");
        ssignals.emplace_back(static_cast<VulkanDeviceSemaphore*>(signal)->GetVkSemaphore());
    }
    vk::PipelineStageFlags mask = vkPipelineStageFlagsFromRHIPipelineStage(desc.stages);
    vk::SubmitInfo info{
        .waitSemaphoreCount = static_cast<uint32_t>(swaits.size()),
        .pWaitSemaphores = swaits.data(),
        .pWaitDstStageMask = &mask,
        .commandBufferCount = static_cast<uint32_t>(cmds.size()),
        .pCommandBuffers = cmds.data(),
        .signalSemaphoreCount = static_cast<uint32_t>(ssignals.size()),
        .pSignalSemaphores = ssignals.data()
    };
    vk::TimelineSemaphoreSubmitInfo tinfo{
        .waitSemaphoreValueCount = static_cast<uint32_t>(wait_values.size()),
        .pWaitSemaphoreValues = wait_values.data(),
        .signalSemaphoreValueCount = static_cast<uint32_t>(signal_values.size()),
        .pSignalSemaphoreValues = signal_values.data()
    };
    if (wait_values.size() || signal_values.size())
        info.setPNext(&tinfo);
    m_queue.submit(
        info,
        desc.fence ? static_cast<VulkanDeviceFence*>(desc.fence)->GetVkFence() : vk::Fence(nullptr)
    );
}
void VulkanDeviceQueue::Present(PresentDesc const& desc) const {
    CHECK(m_device.GetDeviceQueue(RHIDeviceQueueType::Present) == this && "Present called on a queue that is not a present queue");
    Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
    vk::SwapchainKHR swapchain = static_cast<VulkanSwapchain*>(desc.swapchain)->GetVkSwapchain();
    Core::StlVector<vk::Semaphore> swaits(alloc.Ptr());
    swaits.reserve(desc.waits.size());
    for (auto& wait : desc.waits) {
        CHECK(!wait->m_is_timeline && "Present() wait must be Binary semaphores");
        swaits.emplace_back(static_cast<const VulkanDeviceSemaphore*>(wait)->GetVkSemaphore());
    }
    vk::PresentInfoKHR present_info{
        .waitSemaphoreCount = static_cast<uint32_t>(swaits.size()),
        .pWaitSemaphores = swaits.data(),
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &desc.image_index,
    };
    try {
        auto res = m_queue.presentKHR(present_info);
        CHECK(res == vk::Result::eSuccess && "failed to present");
    } catch (std::exception&) {
        // XXX: Not always the case e.g. device lost        
        throw RHISwapchainResizeException{};
    }
}

void VulkanDeviceQueue::DebugSetObjectName(const char* name) {
    VkQueue handle = *m_queue;
    m_device.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eQueue,
        .objectHandle = (uint64_t)(handle),
        .pObjectName = name
        });
}

RHIDeviceScopedObjectHandle<RHIBuffer> VulkanDevice::CreateBuffer(RHIBufferDesc const& desc) {
    return { this, m_storage.CreateObject<VulkanBuffer>(*this, desc) };
}
RHIBuffer* VulkanDevice::GetBuffer(Handle handle) const {
    return m_storage.GetObjectPtr<RHIBuffer>(handle);
}
void VulkanDevice::DestroyBuffer(Handle handle) {
    m_storage.DestroyObject(handle);
}

RHIDeviceScopedObjectHandle<RHITexture> VulkanDevice::CreateTexture(RHITextureDesc const& desc) {
    return { this, m_storage.CreateObject<VulkanTexture>(*this, desc) };
}
RHITexture* VulkanDevice::GetImage(Handle handle) const {
    return m_storage.GetObjectPtr<RHITexture>(handle);
}
void VulkanDevice::DestroyImage(Handle handle) {
    m_storage.DestroyObject(handle);
}

void VulkanDeviceDescriptorSetLayout::DebugSetObjectName(const char* name) {
    VkDescriptorSetLayout handle = *m_layout;
    m_device.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eDescriptorSetLayout,
        .objectHandle = (uint64_t)(handle),
        .pObjectName = name
        });
}

VulkanDeviceDescriptorSetLayout::VulkanDeviceDescriptorSetLayout(const VulkanDevice& device, RHIDeviceDescriptorSetLayoutDesc const& desc)
    : m_device(device), RHIDeviceDescriptorSetLayout(device, desc)
{
    Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
    Core::StlVector<vk::DescriptorSetLayoutBinding> bindings(desc.bindings.size(), alloc.Ptr());
    for (size_t i = 0; i < desc.bindings.size(); ++i) {
        auto const& b = desc.bindings[i];
        bindings[i] = vk::DescriptorSetLayoutBinding{
            .binding = static_cast<uint32_t>(i),
            .descriptorType = vkDescriptorTypeFromRHIDescriptorType(b.type),
            .descriptorCount = b.count,
            .stageFlags = vkShaderStageFlagsFromRHIShaderStage(b.stage)
        };
    }
    m_layout = vk::raii::DescriptorSetLayout(
        m_device.GetVkDevice(),
        vk::DescriptorSetLayoutCreateInfo{
            .bindingCount = static_cast<uint32_t>(bindings.size()),
            .pBindings = bindings.data()
        },
        m_device.GetVkAllocatorCallbacks()
    );
    CHECK_MSG(m_layout != nullptr, "failed to create Vulkan descriptor set layout");
}
RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout> VulkanDevice::CreateDescriptorSetLayout(RHIDeviceDescriptorSetLayoutDesc const& desc) {
    return { this, m_storage.CreateObject<VulkanDeviceDescriptorSetLayout>(*this, desc) };
}
RHIDeviceDescriptorSetLayout* VulkanDevice::GetDescriptorSetLayout(Handle handle) const {
    return m_storage.GetObjectPtr<RHIDeviceDescriptorSetLayout>(handle);
}
void VulkanDevice::DestroyDescriptorSetLayout(Handle handle) {
    m_storage.DestroyObject(handle);
}

#include "Descriptor.hpp"
RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> VulkanDevice::CreateDescriptorPool(RHIDeviceDescriptorPool::PoolDesc const& desc) {
    return { this, m_storage.CreateObject<VulkanDeviceDescriptorPool>(*this, desc) };
}
RHIDeviceDescriptorPool* VulkanDevice::GetDescriptorPool(Handle handle) const {
    return m_storage.GetObjectPtr<RHIDeviceDescriptorPool>(handle);
}
void VulkanDevice::DestroyDescriptorPool(Handle handle) {
    m_storage.DestroyObject(handle);
}


void VulkanDeviceSampler::DebugSetObjectName(const char* name) {
    VkSampler handle = *m_sampler;
    m_device.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eSampler,
        .objectHandle = (uint64_t)(handle),
        .pObjectName = name
        });
}

VulkanDeviceSampler::VulkanDeviceSampler(const VulkanDevice& device, RHIDeviceSampler::SamplerDesc const& desc):
    RHIDeviceSampler(device, desc), m_device(device) {
    auto vkFilterFromRHISamplerFilter = [](RHIDeviceSampler::SamplerDesc::Filter::Type filter) -> vk::Filter {
        using enum RHIDeviceSampler::SamplerDesc::Filter::Type;
        switch (filter) {
            case NearestNeighbor: return vk::Filter::eNearest;
            case Linear:  return vk::Filter::eLinear;
            case Cubic: return vk::Filter::eCubicEXT; // TODO: check if supported
            default: throw std::runtime_error("unsupported sampler filter");
        }
    };
    auto vkSamplerAddressModeFromRHIAddressMode = [](RHIDeviceSampler::SamplerDesc::AddressMode::Mode mode) -> vk::SamplerAddressMode {
        switch (mode) {
            case RHIDeviceSampler::SamplerDesc::AddressMode::Repeat: return vk::SamplerAddressMode::eRepeat;
            case RHIDeviceSampler::SamplerDesc::AddressMode::MirroredRepeat: return vk::SamplerAddressMode::eMirroredRepeat;
            case RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge: return vk::SamplerAddressMode::eClampToEdge;
            case RHIDeviceSampler::SamplerDesc::AddressMode::ClampToBorder: return vk::SamplerAddressMode::eClampToBorder;
            case RHIDeviceSampler::SamplerDesc::AddressMode::MirrorClampToEdge: return vk::SamplerAddressMode::eMirrorClampToEdge;
            default: throw std::runtime_error("unsupported sampler address mode");
        }
    };
    m_sampler = vk::raii::Sampler(
        m_device.GetVkDevice(),
        vk::SamplerCreateInfo{
            .magFilter = vkFilterFromRHISamplerFilter(desc.filter.mag_filter),
            .minFilter = vkFilterFromRHISamplerFilter(desc.filter.min_filter),
            .mipmapMode = desc.mipmap.mipmap_mode == RHIDeviceSampler::SamplerDesc::Mipmap::Linear
                ? vk::SamplerMipmapMode::eLinear
                : vk::SamplerMipmapMode::eNearest,
            .addressModeU = vkSamplerAddressModeFromRHIAddressMode(desc.address_mode.u),
            .addressModeV = vkSamplerAddressModeFromRHIAddressMode(desc.address_mode.v),
            .addressModeW = vkSamplerAddressModeFromRHIAddressMode(desc.address_mode.w),
            .mipLodBias = desc.mipmap.bias,
            .anisotropyEnable = desc.anisotropy.enable,
            .maxAnisotropy = desc.anisotropy.max_level,                                    
            .minLod = desc.lod.min,
            .maxLod = desc.lod.max
        },
        m_device.GetVkAllocatorCallbacks()
    );
    CHECK_MSG(m_sampler != nullptr, "failed to create Vulkan sampler");
}
RHIDeviceScopedObjectHandle<RHIDeviceSampler> VulkanDevice::CreateSampler(RHIDeviceSampler::SamplerDesc const& desc) {
    return { this, m_storage.CreateObject<VulkanDeviceSampler>(*this, desc) };
}
RHIDeviceSampler* VulkanDevice::GetSampler(Handle handle) const {
    return m_storage.GetObjectPtr<RHIDeviceSampler>(handle);
}
void VulkanDevice::DestroySampler(Handle handle) {
    m_storage.DestroyObject(handle);
}
