FContext* GContext = nullptr;

void ResetEditorFrameScratch(FContext* context)
{
    context = context ? context : GContext;
    if (!context || !context->editorFrameArena || !context->editorFrameScratch)
        return;
    context->editorFrameScratch->Reset(static_cast<Arena>(*context->editorFrameArena));
}

void UpdateSwapchain(FContext* context)
{
    constexpr RHISurfaceFormat kFormatPreferenceListHDR[] = {
        {RHIResourceFormat::A2B10G10R10Unorm, RHIColorSpace::Hdr10St2084},
        {RHIResourceFormat::A2R10G10B10Unorm, RHIColorSpace::Hdr10St2084},
        {RHIResourceFormat::R8G8B8A8Unorm, RHIColorSpace::SrgbNonLinear},
        {RHIResourceFormat::B8G8R8A8Unrom, RHIColorSpace::SrgbNonLinear}
    };
    constexpr RHISurfaceFormat kFormatPreferenceListSDR[] = {
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
    auto firstHDR = Ranges::FirstOf(Views::all(kFormatPreferenceListHDR) |
                                  Views::filter(Ranges::ContainedBy(supportedFormats)));
    auto firstSDR = Ranges::FirstOf(Views::all(kFormatPreferenceListSDR) |
                                  Views::filter(Ranges::ContainedBy(supportedFormats)));
    auto format = context->enableHDR ? firstHDR : firstSDR;
    if (!format.has_value() && firstSDR.has_value())
    {
        LOG(RenderApplication, LogError, "Fallback to SDR {} as HDR is not supported", firstSDR.value().format);
        format = firstSDR;
    }
    CHECK_MSG(format.has_value(), "No supported swapchain format found!");
    auto present =
        Ranges::FirstOf(Views::all(kPresentModePreferenceList) |
                        Views::filter(Ranges::ContainedBy(context->device->GetSwapchainSupportedPresentModes())));
    CHECK_MSG(present.has_value(), "No supported presentation mode found!");
    LOG(Editor, LogDebug, "Selected swapchain format: {} with color space: {}", format.value().format, format.value().colorSpace);
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
    context->editorFrameArena = ConstructUnique<ScopedArena>(allocator, allocator, kEditorFrameScratchSize);
    context->editorFrameScratch = ConstructUnique<AllocatorStack>(
        allocator, static_cast<Arena>(*context->editorFrameArena));
    context->window = window;
    context->application = ConstructBase<RHIApplication, VulkanApplication>(allocator, allocator);
    context->device = context->application->CreateDevice(deviceDesc, window);
    context->psoCache = context->device->CreatePipelineCache({});
    context->gpuScene = Construct<GPUScene>(allocator, context, GPUScene::GPUSceneDesc{
        .primitiveBudget = 2048 * (1u << 20) // 2048 MB
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
