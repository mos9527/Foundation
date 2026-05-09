#include <algorithm>

FContext* GContext = nullptr;

static bool WindowHDRStateEqual(FContext::WindowHDRState const& lhs, FContext::WindowHDRState const& rhs)
{
    return lhs.propertiesAvailable == rhs.propertiesAvailable &&
           lhs.enabled == rhs.enabled &&
           lhs.sdrWhiteLevel == rhs.sdrWhiteLevel &&
           lhs.sdrWhiteNits == rhs.sdrWhiteNits &&
           lhs.headroom == rhs.headroom &&
           lhs.peakNits == rhs.peakNits;
}

bool UpdateWindowHDRState(FContext* context)
{
    context = context ? context : GContext;
    if (!context || !context->window)
        return false;

    FContext::WindowHDRState next{};
#if SDL_VERSION_ATLEAST(3, 2, 0)
    SDL_PropertiesID props = SDL_GetWindowProperties(context->window);
    if (props != 0 && SDL_HasProperty(props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN))
    {
        next.propertiesAvailable = true;
        next.enabled = SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
        next.sdrWhiteLevel = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
        next.headroom = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
        next.sdrWhiteNits = next.sdrWhiteLevel * 80.0f;
        next.peakNits = next.sdrWhiteNits * next.headroom;
    }
#endif

    bool previousEnableHDR = context->enableHDR;
    bool changed = !WindowHDRStateEqual(context->windowHDR, next);
    context->windowHDR = next;
    if (context->windowHDR.propertiesAvailable)
        context->enableHDR = context->windowHDR.enabled;
    bool hdrOutputChanged = previousEnableHDR != context->enableHDR;
    if (changed)
    {
        if (context->windowHDR.propertiesAvailable)
        {
            LOG(Editor, LogInfo, "SDL HDR window state: {} (SDR white {:.1f} nits approx, headroom {:.2f}x, peak {:.1f} nits approx)",
                context->windowHDR.enabled ? "enabled" : "disabled",
                context->windowHDR.sdrWhiteNits,
                context->windowHDR.headroom,
                context->windowHDR.peakNits);
        }
        else
        {
            LOG(Editor, LogInfo, "SDL HDR window properties unavailable; using Vulkan surface capabilities and manual HDR toggle");
        }
    }
    return changed || hdrOutputChanged;
}

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
        {RHIResourceFormat::A2R10G10B10Unorm, RHIColorSpace::Hdr10St2084}
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
    bool hdrBlockedByWindow = context->windowHDR.propertiesAvailable && !context->windowHDR.enabled;
    auto format = context->enableHDR && !hdrBlockedByWindow ? firstHDR : firstSDR;
    if (context->enableHDR && hdrBlockedByWindow)
        LOG(RenderApplication, LogWarn, "HDR output requested, but SDL reports no HDR headroom for this window; using SDR");
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
    UpdateWindowHDRState(context);
    context->application = ConstructBase<RHIApplication, VulkanApplication>(allocator, allocator);
    context->device = context->application->CreateDevice(deviceDesc, window);
    context->psoCache = context->device->CreatePipelineCache({});
    const size_t requestedPrimitiveBudget = 2048ull * (1ull << 20); // 2048 MB
    const size_t primitiveBudget = std::min(requestedPrimitiveBudget,
                                            context->device->GetCapabilities().maxStorageBufferRange) & ~size_t(3);
    context->gpuScene = Construct<GPUScene>(allocator, context, GPUScene::GPUSceneDesc{
        .primitiveBudget = static_cast<uint32_t>(primitiveBudget),
        .curveAABBBudget = 512 * (1u << 20) // 512 MB
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
