FEditorContext* GEditor = nullptr;

void UpdateSwapchain(FEditorContext* context)
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

FEditorContext* CreateEditorContext(SDL_Window* window, Allocator* allocator)
{
    auto* context = Construct<FEditorContext>(allocator);
    context->allocator = allocator;
    context->window = window;
    context->application = ConstructUniqueBase<RHIApplication, VulkanApplication>(allocator, allocator);
    context->device = context->application->CreateDevice({}, window);
    UpdateSwapchain(context);
    GEditor = context;
    return context;
}

void DestroyEditorContext(FEditorContext* context)
{
    context = context ? context : GEditor;
    if (!context)
        return;
    Destruct(context->allocator, context);
}
