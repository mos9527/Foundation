#define VMA_IMPLEMENTATION
#include <queue>
#include <vk_mem_alloc.h>

using namespace Foundation::Core;
using namespace Foundation::RHI;
const char* kVulkanDeviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_MESH_SHADER_EXTENSION_NAME,
                                         VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME};

const char* kVulkanDeviceTypes[] = {"Other", "Integrated GPU", "Discrete GPU", "Virtual GPU", "CPU"};

Allocator* VulkanDevice::GetAllocator() const { return mApp.GetAllocator(); }
vk::AllocationCallbacks const& VulkanDevice::GetVkAllocatorCallbacks() const { return mApp.GetVkAllocatorCallbacks(); }

VulkanDevice::VulkanDevice(VulkanApplication const& app, vk::raii::PhysicalDevice physicalDevice, SDL_Window* window) :
    RHIDevice(app), mApp(app), mWindow(window), mPhysicalDevice(std::move(physicalDevice)),
    mSwapchainFormats(GetAllocator()), mSwapchainPresentModes(GetAllocator()), mStorage(GetAllocator())
{
    auto families = mPhysicalDevice.getQueueFamilyProperties();
    // Find queues
    // Graphics, Compute, Transfer should be preferably mutually exclusive
    // NOTE: We never used dedicated transfer in the Renderer - offloading to compute is more than enough for such
    // tasks.
    Pair<uint32_t, uint32_t> graphics{kInvalidQueueIndex, kInvalidQueueIndex},
        compute{kInvalidQueueIndex, kInvalidQueueIndex}, transfer{kInvalidQueueIndex, kInvalidQueueIndex};
    Array<uint32_t, 256> queueCounts{};
    for (size_t i = 0; i < families.size(); ++i)
    {
        auto& family = families[i];
        if (family.queueCount && family.queueFlags & vk::QueueFlagBits::eGraphics &&
            graphics.first == kInvalidQueueIndex)
        {
            graphics = {i, queueCounts[i]++};
            family.queueCount--;
        }
        if (family.queueCount && family.queueFlags & vk::QueueFlagBits::eCompute && compute.first == kInvalidQueueIndex)
        {
            compute = {i, queueCounts[i]++};
            family.queueCount--;
        }
        if (family.queueCount && family.queueFlags & vk::QueueFlagBits::eTransfer &&
            transfer.first == kInvalidQueueIndex)
        {
            transfer = {i, queueCounts[i]++};
            family.queueCount--;
        }
    }
    CHECK(graphics.first != kInvalidQueueIndex);
    CHECK(compute.first != kInvalidQueueIndex);
    CHECK(transfer.first != kInvalidQueueIndex);
    if (window)
    {
        // Check for a present queue
        VkSurfaceKHR surface;
        CHECK_MSG(SDL_Vulkan_CreateSurface(window, *mApp.GetVkInstance(),
                                           nullptr /* Doesn't work well with SDL_DestroyWindow */, &surface),
                  "failed to create window surface: {}", SDL_GetError());
        mSurface = vk::raii::SurfaceKHR(mApp.GetVkInstance(), surface);
        // Having present and graphics queues as the same avoids copies and is typically the case
        // - https://github.com/KhronosGroup/Vulkan-Hpp/blob/main/RAII_Samples/05_InitSwapchain/05_InitSwapchain.cpp#L45
        // - https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/master/src/VulkanSample.cpp#L1850
        CHECK(mPhysicalDevice.getSurfaceSupportKHR(graphics.first, *mSurface));
    }
    // Create the device queues
    Vector<vk::DeviceQueueCreateInfo> queue_info(GetAllocator());
    const float priority = 1.0f;
    for (uint32_t i = 0; i < families.size(); ++i)
    {
        if (queueCounts[i])
            queue_info.emplace_back(vk::DeviceQueueCreateInfo{
                .queueFamilyIndex = i,
                .queueCount = queueCounts[i],
                .pQueuePriorities = &priority // All queues have the same priority
            });
    }
    vk::StructureChain<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
                       vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features,
                       vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT, vk::PhysicalDeviceMeshShaderFeaturesEXT>
        featureChain = {
            {.features = {.samplerAnisotropy = true,
                          .fragmentStoresAndAtomics = true,
                          .shaderInt16 = true}}, // vk::PhysicalDeviceFeatures2
            {.storageBuffer16BitAccess = true,
             .uniformAndStorageBuffer16BitAccess = true,
             .shaderDrawParameters = true}, // vk::PhysicalDeviceVulkan11Features
            {.drawIndirectCount = true,
             .storageBuffer8BitAccess = true,
             .uniformAndStorageBuffer8BitAccess = true,
             .shaderFloat16 = true,
             .shaderInt8 = true,
             .descriptorBindingSampledImageUpdateAfterBind = true,
             .runtimeDescriptorArray = true,
             .samplerFilterMinmax = true,
             .scalarBlockLayout = true,
             .uniformBufferStandardLayout = true,
             .timelineSemaphore = true}, // vk::PhysicalDeviceVulkan12Features
            {.synchronization2 = true,
             .dynamicRendering = true,
             .shaderIntegerDotProduct = true}, // vk::PhysicalDeviceVulkan13Features
            {.extendedDynamicState = true}, // vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT
            {.taskShader = true, .meshShader = true} // vk::PhysicalDeviceMeshShaderFeaturesEXT
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
    mQueues->graphics = mQueues->storage.CreateObject<VulkanDeviceQueue>(*this, graphics.first, graphics.second);
    mQueues->compute = mQueues->storage.CreateObject<VulkanDeviceQueue>(*this, compute.first, compute.second);
    mQueues->transfer = mQueues->storage.CreateObject<VulkanDeviceQueue>(*this, transfer.first, transfer.second);
    // Initialize VMA
    const VmaAllocatorCreateInfo allocator_info{
        .physicalDevice = *mPhysicalDevice,
        .device = *mDevice,
        .pAllocationCallbacks = reinterpret_cast<const VkAllocationCallbacks*>(&GetVkAllocatorCallbacks()),
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

VulkanDevice::~VulkanDevice()
{
    mStorage.Destroy();
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

void VulkanDevice::DebugSetObjectName(const char* name)
{
    VkDevice handle = *mDevice;
    mDevice.setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::eDevice,
                                        .objectHandle = reinterpret_cast<uint64_t>(handle),
                                        .pObjectName = name});
}

void VulkanDeviceQueue::WaitIdle() const { mQueue.waitIdle(); }
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
        mDevice.GetVkAllocatorCallbacks());
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
    RHIDeviceQueryPool(device, desc), mDevice(device), mTimestampResults(device.GetAllocator())
{
    vk::QueryPoolCreateInfo createInfo = {.queryCount = desc.count};
    switch (desc.type)
    {
    case QueryPoolDesc::Timestamp:
        createInfo.queryType = vk::QueryType::eTimestamp;
        mTimestampResults.resize(desc.count);
        break;
    }
    mQueryPool = vk::raii::QueryPool(mDevice.GetVkDevice(), createInfo, mDevice.GetVkAllocatorCallbacks());
    CHECK_MSG(mQueryPool != nullptr, "failed to create Vulkan query pool");
}
Span<const uint64_t> VulkanDeviceQueryPool::GetTimestampResults(bool wait)
{
    CHECK_MSG(mDesc.type == QueryPoolDesc::Timestamp, "GetTimestampResults called on non-timestamp query pool");
    vkGetQueryPoolResults(*mDevice.GetVkDevice(), *mQueryPool, 0, mDesc.count,
                          sizeof(uint64_t) * mDesc.count, mTimestampResults.data(), sizeof(uint64_t),
                          wait ? VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_64_BIT : VK_QUERY_RESULT_64_BIT);
    return mTimestampResults;
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
    mSampler = vk::raii::Sampler(mDevice.GetVkDevice(), sampler, mDevice.GetVkAllocatorCallbacks());
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
