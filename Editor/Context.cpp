FContext* GContext = nullptr;

void UpdateSwapchain(FContext* context)
{
    constexpr RHIResourceFormat kFormatPreferenceList[] = {
        RHIResourceFormat::R8G8B8A8Unorm, RHIResourceFormat::B8G8R8A8Unrom, RHIResourceFormat::R8G8B8A8Srgb,
        RHIResourceFormat::B8G8R8A8Srgb};
    constexpr RHISwapchainPresentMode kPresentModePreferenceList[] = {
        RHISwapchainPresentMode::Mailbox, RHISwapchainPresentMode::Tearing, RHISwapchainPresentMode::Fifo};
    int w, h;
    SDL_GetWindowSizeInPixels(context->window, &w, &h);
    LOG(RenderApplication, LogDebug, "Creating swapchain ({}x{})", w, h);
    context->device->WaitIdle();
    if (context->swapchain)
        context->swapchain.Reset();
    auto format = Ranges::FirstOf(Views::all(kFormatPreferenceList) |
                                  Views::filter(Ranges::ContainedBy(context->device->GetSwapchainSupportedFormats())));
    auto present =
        Ranges::FirstOf(Views::all(kPresentModePreferenceList) |
                        Views::filter(Ranges::ContainedBy(context->device->GetSwapchainSupportedPresentModes())));
    CHECK_MSG(format.has_value(), "No supported swapchain format found!");
    LOG(Editor, LogDebug, "Selected swapchain format: {}", format.value());
    CHECK_MSG(present.has_value(), "No supported presentation mode found!");
    LOG(Editor, LogDebug, "Selected swapchain present mode: {}", present.value());
    context->swapchain = context->device->CreateSwapchain(RHISwapchain::SwapchainDesc{
        .format = format.value(),
        .extents = RHIExtent3D{w, h, 1},
        .minBufferCount = 3,
        .presentMode = present.value(),
    });
}

FContext* CreateContext(SDL_Window* window, Allocator* allocator)
{
    auto* context = Construct<FContext>(allocator);
    context->allocator = allocator;
    context->window = window;
    context->application = ConstructBase<RHIApplication, VulkanApplication>(allocator, allocator);
    context->device = context->application->CreateDevice({}, window);
    context->gpuScene = Construct<GPUScene>(allocator, context, GPUScene::GPUSceneDesc{
        .primitiveBudget = 1024 * (1u << 20) // 1 GB
    });
    UpdateSwapchain(context);
    ImGui_ImplFoundation_SetupContextWithDefaultStyles();
    ImGui_ImplFoundation_Init(context->device.Get(), context->window);
    return GContext = context;
}

void DestroyContext(FContext* context)
{
    context = context ? context : GContext;
    if (!context)
        return;
    ImGui_ImplFoundation_Shutdown();
    SDL_DestroyWindow(context->window);
    Destruct(context->allocator, context->renderer);
    Destruct(context->allocator, context->gpuScene);
    context->swapchain.Reset();
    context->device.Reset();
    Destruct(context->allocator, context->application);
    Destruct(context->allocator, context);
}
