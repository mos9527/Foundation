using namespace Foundation::Core;
using namespace Foundation::RHI;
vk::SwapchainCreateInfoKHR VulkanSwapchain::vkSwapchainCreateInfoFromSwapchainDesc(SwapchainDesc desc)
{
    auto const& surface = mDevice.GetVkSurface();
    auto surface_caps = mDevice.GetVkPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
    auto present_modes = mDevice.GetVkPhysicalDevice().getSurfacePresentModesKHR(surface);
    // Validate requested parameters
    desc.extents.x = std::clamp(desc.extents.x, surface_caps.minImageExtent.width, surface_caps.maxImageExtent.width);
    desc.extents.y = std::clamp(desc.extents.y, surface_caps.minImageExtent.height, surface_caps.maxImageExtent.height);
    if (desc.minBufferCount < surface_caps.minImageCount)
        desc.minBufferCount = surface_caps.minImageCount;
    if (surface_caps.maxImageCount > 0 && desc.minBufferCount > surface_caps.maxImageCount)
        desc.minBufferCount = surface_caps.maxImageCount;
    CHECK_MSG(
        Ranges::find(present_modes, vkPresentModeFromSwapchainDesc(desc.presentMode)) != present_modes.end(),
        "Swapchain present mode {} not supported",
        static_cast<uint32_t>(desc.presentMode));
    CHECK_MSG(
        desc.minBufferCount >= surface_caps.minImageCount,
        "Swapchain min buffer count {} not supported (min {})",
        desc.minBufferCount, surface_caps.minImageCount
    );
    vk::Format vk_format = vkFormatFromRHIFormat(desc.format);
    Optional<vk::ColorSpaceKHR> color_space;
    auto formats = mDevice.GetVkPhysicalDevice().getSurfaceFormatsKHR(surface);
    for (auto const& fmt : formats) {
        if (fmt.format == vk_format) {
            color_space = fmt.colorSpace;
            break;
        }
    }
    CHECK_MSG(color_space.has_value(), "Swapchain format {} not supported", desc.format);
    vk::SwapchainCreateInfoKHR create_info{
        .surface = surface,
        .minImageCount = desc.minBufferCount,
        .imageFormat = vk_format,
        .imageColorSpace = color_space.value(),
        .imageExtent = vk::Extent2D(desc.extents.x, desc.extents.y),
        .imageArrayLayers = 1, // 1 layer for 2D images
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eStorage,
        // ^^ AMD uses CS to clear themselves - not sure about NV drivers
        .imageSharingMode = vk::SharingMode::eExclusive, // Exclusive mode by default
        .preTransform = surface_caps.currentTransform,
        .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque, // Opaque composite alpha
        .presentMode = vkPresentModeFromSwapchainDesc(desc.presentMode),
        .clipped = VK_TRUE, // Clipped presentation
        .oldSwapchain = mSwapchain // No old swapchain
    };
    return create_info;
}
void VulkanSwapchain::Instantiate() {
    auto const& device = mDevice.GetVkDevice();
    auto create_info = vkSwapchainCreateInfoFromSwapchainDesc(mDesc);
    mImages.reset();
    mImages = ConstructUnique<RHIObjectPool<VulkanTexture>>(mDevice.GetAllocator(), mDevice.GetAllocator());
    mImagesPtrs.clear();
    mSwapchain = vk::raii::SwapchainKHR(device, create_info, mDevice.GetVkAllocatorCallbacks());
    auto images = mSwapchain.getImages();
    for (auto& image : images) {
        const Handle handle = mImages->CreateObject<VulkanTexture>(mDevice, RHITextureDesc{}, vk::raii::Image(device, image, mDevice.GetVkAllocatorCallbacks()), true /*shared=true*/);
        mImagesPtrs.push_back(mImages->GetObjectPtr(handle));
    }
    // Update actual extents
    mDesc.extents.x = create_info.imageExtent.width;
    mDesc.extents.y = create_info.imageExtent.height;
    mDesc.minBufferCount = create_info.minImageCount;
}
VulkanSwapchain::VulkanSwapchain(const VulkanDevice& device, SwapchainDesc const& desc)
    : RHISwapchain(device, desc), mDevice(device), mImagesPtrs(device.GetAllocator()) {
    Instantiate();
}
Span<RHITexture* const> VulkanSwapchain::GetImages() const {
    return mImagesPtrs;
}
RHIExtent2D VulkanSwapchain::GetExtents() const
{
    return mDesc.extents;
}
uint32_t VulkanSwapchain::GetNextImage(uint64_t timeout_ns, RHIDeviceRef<RHIDeviceSemaphore> semaphore, RHIDeviceRef<RHIDeviceFence> fence)
{
    auto [result, index] = mSwapchain.acquireNextImage(
        timeout_ns,
        semaphore ? semaphore.Get<VulkanDeviceSemaphore>()->GetVkSemaphore() : vk::Semaphore(),
        fence ? fence.Get<VulkanDeviceFence>()->GetVkFence() : vk::Fence()
    );
    switch (result)
    {
    case vk::Result::eErrorOutOfDateKHR:
    case vk::Result::eSuboptimalKHR:
        // Swapchain resize        
        throw RHISwapchainResizeException();
    default:
        // TODO: Handle other errors?
        break;
    }
    return index;
}

void VulkanSwapchain::DebugSetObjectName(const char* name) {
    VkSwapchainKHR handle = *mSwapchain;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eSwapchainKHR,
        .objectHandle = reinterpret_cast<uint64_t>(handle),
        .pObjectName = name
        });
}
