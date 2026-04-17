FContext* GContext = nullptr;

void UpdateSwapchain(FContext* context)
{
    constexpr RHISurfaceFormat kFormatPreferenceList[] = {
        {RHIResourceFormat::A2B10G10R10Unorm, RHIColorSpace::Hdr10St2084},
        {RHIResourceFormat::R8G8B8A8Unorm, RHIColorSpace::SrgbNonLinear},
        {RHIResourceFormat::B8G8R8A8Unrom, RHIColorSpace::SrgbNonLinear}
    };
    constexpr RHISwapchainPresentMode kPresentModePreferenceList[] = {
        RHISwapchainPresentMode::Mailbox, RHISwapchainPresentMode::Tearing, RHISwapchainPresentMode::Fifo};
    int w, h;
    SDL_GetWindowSizeInPixels(context->window, &w, &h);
    LOG(RenderApplication, LogDebug, "Creating swapchain ({}x{})", w, h);
    context->device->WaitIdle();
    if (context->swapchain)
        context->swapchain.Reset();
    auto supportedFormats = context->device->GetSwapchainSupportedFormats();
    auto format = Ranges::FirstOf(Views::all(kFormatPreferenceList) |
                                  Views::filter(Ranges::ContainedBy(supportedFormats)));
    auto present =
        Ranges::FirstOf(Views::all(kPresentModePreferenceList) |
                        Views::filter(Ranges::ContainedBy(context->device->GetSwapchainSupportedPresentModes())));
    CHECK_MSG(format.has_value(), "No supported swapchain format found!");
    LOG(Editor, LogDebug, "Selected swapchain format: {} with color space: {}", format.value().format, format.value().colorSpace);
    CHECK_MSG(present.has_value(), "No supported presentation mode found!");
    LOG(Editor, LogDebug, "Selected swapchain present mode: {}", present.value());
    context->swapchain = context->device->CreateSwapchain(RHISwapchain::SwapchainDesc{
        .format = format.value().format,
        .colorSpace = format.value().colorSpace,
        .extents = RHIExtent3D{w, h, 1},
        .minBufferCount = 3,
        .presentMode = present.value(),
    });
}

FContext* CreateContext(SDL_Window* window, Allocator* allocator, RHIDevice::DeviceDesc const& deviceDesc)
{
    auto* context = Construct<FContext>(allocator);
    context->allocator = allocator;
    context->window = window;
    context->application = ConstructBase<RHIApplication, VulkanApplication>(allocator, allocator);
    context->device = context->application->CreateDevice(deviceDesc, window);
    context->psoCache = context->device->CreatePipelineCache({});
    context->gpuScene = Construct<GPUScene>(allocator, context, GPUScene::GPUSceneDesc{
        .primitiveBudget = 1024 * (1u << 20) // 1024 MB
    });
    UpdateSwapchain(context);
    return GContext = context;
}

void DestroyContext(FContext* context)
{
    context = context ? context : GContext;
    if (!context)
        return;
    context->device->WaitIdle();
    SDL_DestroyWindow(context->window);
    Destruct(context->allocator, context->renderer);
    Destruct(context->allocator, context->gpuScene);
    context->psoCache.Reset();
    context->swapchain.Reset();
    context->device.Reset();
    Destruct(context->allocator, context->application);
    Destruct(context->allocator, context);
}
