#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>

#include <Core/Bits/StringUtils.hpp>
#include <Core/Logging.hpp>
#include <algorithm>

#include <Platform/RHI/Vulkan/Application.hpp>
#include <Platform/RHI/Vulkan/Device.hpp>
using namespace Foundation::Core;
using namespace Foundation::Platform::RHI;

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

VulkanDevice::VulkanDevice(Window* window, VulkanApplication const& app, const vk::raii::PhysicalDevice& physicalDevice, Core::Allocator* allocator) :
    m_app(app), m_physicalDevice(physicalDevice), m_allocator(allocator), RHIDevice(app),
    m_storage(allocator) {
    Instantiate(window);
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
        "        nonCoherentAtomSize: {}\n",
        properties.driverVersion,
        properties.vendorID,
        properties.deviceID,
        kVulkanDeviceTypes[static_cast<int>(properties.deviceType)],
        properties.deviceName.data(),
        properties.limits.maxMemoryAllocationCount,
        Bits::ByteSizeToString(properties.limits.bufferImageGranularity),
        Bits::ByteSizeToString(properties.limits.nonCoherentAtomSize)
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
        "** Vulkan Allocator Stats for {} **\n"
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
        GetName(),
        deviceLocalHeapCount,
        Bits::ByteSizeToString(deviceLocalHeapSumSize),
        hostVisibleHeapCount,
        Bits::ByteSizeToString(hostVisibleHeapSumSize),
        deviceLocalAndHostVisibleHeapCount,
        Bits::ByteSizeToString(deviceLocalAndHostVisibleHeapSumSize),
        hostVisibleNotHostCoherentTypeCount,
        notDeviceLocalNotHostVisibleTypeCount,
        amdSpecificTypeCount,
        lazilyAllocatedTypeCount
    );
}

VulkanDevice::~VulkanDevice() {
    if (m_allocator) {
        vmaDestroyAllocator(m_vkAllocator);
        m_allocator = nullptr;
    }
}

void VulkanDevice::Instantiate(Window* window) {
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
    m_queues.graphics = find_first(vk::QueueFlagBits::eGraphics, -1, -1);
    m_queues.compute = find_first(vk::QueueFlagBits::eCompute, m_queues.graphics, -1);
    m_queues.transfer = find_first(vk::QueueFlagBits::eTransfer, m_queues.graphics, m_queues.compute);
    if (window) {
        // Check for a present queue
        VkSurfaceKHR surface;
        // NOTE: Creating surfaces is platform-dependent w/ requisite extensions. GLFW does this.
        if (glfwCreateWindowSurface(*m_app.m_instance, window->GetNativeWindow(), nullptr, &surface) != VK_SUCCESS)
            throw std::runtime_error("Failed to create Vulkan surface for window");
        m_surface = vk::raii::SurfaceKHR(m_app.m_instance, surface);
        // Having present and graphics queues as the same avoids copies and is typically the case
        // Most references handles the other case too, however
        // - https://github.com/KhronosGroup/Vulkan-Hpp/blob/main/RAII_Samples/05_InitSwapchain/05_InitSwapchain.cpp#L45
        // - https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/master/src/VulkanSample.cpp#L1850
        if (m_physicalDevice.getSurfaceSupportKHR(m_queues.graphics, *m_surface)) {
            m_queues.present = m_queues.graphics;
        }
        else {
            for (size_t i = 0; i < queues.size(); ++i) {
                if (m_physicalDevice.getSurfaceSupportKHR(static_cast<uint32_t>(i), *m_surface)) {
                    m_queues.present = static_cast<int>(i);
                    break;
                }
            }
        }
    }
    // Sanity check for queue indices
    if (!m_queues.IsValid())
        throw std::runtime_error("Failed to find valid queue indices for Vulkan device");
    // Create the device
    // Gather all unqiue queues we'd need
    std::vector<uint32_t> unique_queues{
        m_queues.graphics,
        m_queues.compute,
        m_queues.transfer,
        m_queues.present
    };
    std::sort(unique_queues.begin(), unique_queues.end());
    unique_queues.erase(
        std::unique(unique_queues.begin(), unique_queues.end()),
        unique_queues.end()
    );
    std::vector<vk::DeviceQueueCreateInfo> queue_info;
    float priority = 1.0f;
    for (auto queueIndex : unique_queues) {
        if (queueIndex == kInvalidQueueIndex)
            continue;
        queue_info.emplace_back(vk::DeviceQueueCreateInfo{
            .queueFamilyIndex = queueIndex,
            .queueCount = 1,
            .pQueuePriorities = &priority // All queues have the same priority
            });
    }
    vk::DeviceCreateInfo device_info{
            .queueCreateInfoCount = static_cast<uint32_t>(queue_info.size()),
            .pQueueCreateInfos = queue_info.data(),
            .enabledLayerCount = 0,
            .enabledExtensionCount = sizeof(kVulkanDeviceExtensions) / sizeof(kVulkanDeviceExtensions[0]),
            .ppEnabledExtensionNames = kVulkanDeviceExtensions
    };
    m_device = vk::raii::Device(m_physicalDevice, device_info);
    // Initialize VMA
    const VmaAllocatorCreateInfo allocator_info{
        .physicalDevice = *m_physicalDevice,
        .device = *m_device,
        .instance = *m_app.m_instance,
        .vulkanApiVersion = m_app.m_vulkanApiVersion
    };
    if (vmaCreateAllocator(&allocator_info, &m_vkAllocator) != VK_SUCCESS) {
        throw std::runtime_error("failed to create VMA for Vulkan device");
    }
    DebugLogAllocatorInfo();
}

#include <Platform/RHI/Vulkan/Swapchain.hpp>
RHIDeviceScopedObjectHandle<RHISwapchain> VulkanDevice::CreateSwapchain(RHISwapchain::SwapchainDesc const& desc) {
    return { this, m_storage.CreateObject<VulkanSwapchain>(*this, desc) };
}
RHISwapchain* VulkanDevice::GetSwapchain(Handle handle) const {
    return m_storage.GetObjectPtr<RHISwapchain>(handle);
};
void VulkanDevice::DestroySwapchain(Handle handle) {
    m_storage.DestoryObject(handle);
}

#include <Platform/RHI/Vulkan/PipelineState.hpp>
RHIDeviceScopedObjectHandle<RHIPipelineState> VulkanDevice::CreatePipelineState(RHIPipelineState::PipelineStateDesc const& desc) {
    return { this, m_storage.CreateObject<VulkanPipelineState>(*this, desc) };
}
RHIPipelineState* VulkanDevice::GetPipelineState(Handle handle) const {
    return m_storage.GetObjectPtr<RHIPipelineState>(handle);
}
void VulkanDevice::DestroyPipelineState(Handle handle) {
    m_storage.DestoryObject(handle);
}

#include <Platform/RHI/Vulkan/Shader.hpp>
RHIDeviceScopedObjectHandle<RHIShaderModule> VulkanDevice::CreateShaderModule(RHIShaderModule::ShaderModuleDesc const& desc) {
    return { this, m_storage.CreateObject<VulkanShaderModule>(*this, desc) };
}
RHIShaderModule* VulkanDevice::GetShaderModule(Handle handle) const {
    return m_storage.GetObjectPtr<RHIShaderModule>(handle);
}
void VulkanDevice::DestroyShaderModule(Handle handle) {
    m_storage.DestoryObject(handle);
}
RHIDeviceScopedObjectHandle<RHIShaderPipelineModule> VulkanDevice::CreateShaderPipelineModule(RHIShaderPipelineModule::ShaderPipelineModuleDesc const& desc, RHIDeviceObjectHandle<RHIShaderModule> shader_module) {
    return { this, m_storage.CreateObject<VulkanShaderPipelineModule>(*this, desc, shader_module) };
}
RHIShaderPipelineModule* VulkanDevice::GetShaderPipelineModule(Handle handle) const {
    return m_storage.GetObjectPtr<RHIShaderPipelineModule>(handle);
}
void VulkanDevice::DestroyShaderPipelineModule(Handle handle) {
    m_storage.DestoryObject(handle);
}
