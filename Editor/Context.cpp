#include "Context.hpp"
#include <RHIVulkan/Application.hpp>

#include <filesystem>
#include <fstream>

FContext* GContext = nullptr;

static String PipelineCachePathForDevice(RHIDevice const& device)
{
    auto key = device.GetPipelineCacheKey();
    return device.mApp.ResolveRelativePathBase(Format("Editor-Cache.pso", key.high, key.low));
}

static Vector<unsigned char> LoadPipelineCacheBytes(StringView path, Allocator* allocator)
{
    Vector<unsigned char> data(allocator);
    if (!std::filesystem::exists(path.data()))
        return data;

    std::ifstream file(path.data(), std::ios::binary | std::ios::ate);
    if (!file)
    {
        LOG(Editor, LogWarn, "Failed to open pipeline cache file for reading: {}", path);
        return data;
    }

    auto fileSize = file.tellg();
    if (fileSize <= std::streampos(0))
        return data;
    auto size = static_cast<size_t>(fileSize);
    data.resize(size);
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
    {
        LOG(Editor, LogWarn, "Failed to read pipeline cache file: {}", path);
        data.clear();
    }
    return data;
}

static void SavePipelineCache(FContext const& context)
{
    if (!context.psoCache || context.psoCachePath.empty())
        return;

    size_t size = context.psoCache->GetSerializedDataSize();
    if (size == 0)
        return;

    Vector<unsigned char> data(size, context.allocator);
    size_t written = context.psoCache->WriteSerializedData(data);
    if (written == 0)
        return;
    data.resize(written);
    std::filesystem::path path(context.psoCachePath);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        LOG(Editor, LogWarn, "Failed to open pipeline cache file for writing: {}", context.psoCachePath);
        return;
    }
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file)
    {
        LOG(Editor, LogWarn, "Failed to write pipeline cache file: {}", context.psoCachePath);
    }
    else
    {
        LOG(Editor, LogInfo, "Saved pipeline cache: {} ({} bytes)", context.psoCachePath, data.size());
    }
}

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
    if (!context->surface)
        context->surface = context->device->CreateSurface(RHISurface::SurfaceDesc{.windowHandle = context->window});
    auto supportedFormats = context->surface->GetSupportedFormats();
    auto firstHDR = Ranges::FirstOf(std::views::all(kFormatPreferenceListHDR) |
                                  std::views::filter(Ranges::ContainedBy(supportedFormats)));
    auto firstSDR = Ranges::FirstOf(std::views::all(kFormatPreferenceListSDR) |
                                  std::views::filter(Ranges::ContainedBy(supportedFormats)));
    bool hdrBlockedByWindow = context->windowHDR.propertiesAvailable && !context->windowHDR.enabled;
    auto format = context->enableHDR && !hdrBlockedByWindow ? firstHDR : firstSDR;
    if (context->enableHDR && hdrBlockedByWindow)
        LOG(RenderApplication, LogWarn, "HDR output requested, but SDL reports no HDR headroom for this window; using SDR");
    if (!format.has_value() && firstSDR.has_value())
    {
        LOG(RenderApplication, LogError, "Fallback to SDR {} as HDR is not supported", firstSDR.Get().format);
        format = firstSDR;
    }
    CHECK_MSG(format.has_value(), "No supported swapchain format found!");
    auto present =
        Ranges::FirstOf(std::views::all(kPresentModePreferenceList) |
                        std::views::filter(Ranges::ContainedBy(context->surface->GetSupportedPresentModes())));
    CHECK_MSG(present.has_value(), "No supported presentation mode found!");
    LOG(Editor, LogDebug, "Selected swapchain format: {} with color space: {}", format.Get().format,
        format.Get().colorSpace);
    LOG(Editor, LogDebug, "Selected swapchain present mode: {}", present.Get());
    context->swapchain = context->device->CreateSwapchain(RHISwapchain::SwapchainDesc{
        .format = format.Get().format,
        .colorSpace = format.Get().colorSpace,
        .extents = RHIExtent3D{w, h, 1},
        .minBufferCount = 3,
        .presentMode = present.Get(),
        .surface = context->surface,
    });
}

FContext* CreateContext(SDL_Window* window, Allocator* allocator, RHIDevice::DeviceDesc const& deviceDesc)
{
    auto* context = Construct<FContext>(allocator);
    context->allocator = allocator;
    unsigned const hw = std::thread::hardware_concurrency();
    size_t const workers = hw > 1u ? static_cast<size_t>(hw - 1u) : 1u;
    context->jobs = ConstructUnique<JobSystem>(allocator, JobSystemDesc{
        .workerCount = workers,
        .maxJobs = 4096,
        .maxBarriers = 64,
        .readyQueueSize = 4096,
        .allocator = allocator,
        .name = "EditorJob",
    });
    context->editorFrameArena = ConstructUnique<ScopedArena>(allocator, allocator, kEditorFrameScratchSize);
    context->editorFrameScratch = ConstructUnique<AllocatorStack>(
        allocator, static_cast<Arena>(*context->editorFrameArena));
    context->window = window;
    UpdateWindowHDRState(context);
    context->application = ConstructBase<RHIApplication, VulkanApplication>(allocator, allocator);
    context->device = context->application->CreateDevice(deviceDesc);
    context->psoCachePath = PipelineCachePathForDevice(*context->device.Get());
    
    // Clear stale caches
    {
        std::filesystem::path cacheDir = std::filesystem::path(context->psoCachePath).parent_path();
        if (std::filesystem::exists(cacheDir))
        {
            for (auto const& entry : std::filesystem::directory_iterator(cacheDir))
            {
                if (entry.is_regular_file() && entry.path() != std::filesystem::path(context->psoCachePath))
                {
                    auto filename = entry.path().filename().string();
                    if (filename.starts_with("pso-cache-") && filename.ends_with(".bin"))
                    {
                        LOG(Editor, LogInfo, "Clearing stale pipeline cache: {}", entry.path().string());
                        std::error_code ec;
                        std::filesystem::remove(entry.path(), ec);
                    }
                }
            }
        }
    }

    auto psoCacheBytes = LoadPipelineCacheBytes(context->psoCachePath, allocator);
    context->psoCache = context->device->CreatePipelineCache({
        .initialData = Span<const unsigned char>(psoCacheBytes.data(), psoCacheBytes.size())
    });
    LOG(Editor, LogInfo, "Pipeline cache {}: {} ({} bytes)",
        context->psoCache->GetImportStatus(),
        context->psoCachePath,
        psoCacheBytes.size());
    UpdateSwapchain(context);
    return GContext = context;
}

void DestroyContext(FContext* context)
{
    context = context ? context : GContext;
    if (!context)
        return;
    context->device->WaitIdle();
    SavePipelineCache(*context);
    SDL_DestroyWindow(context->window);
    Destruct(context->allocator, context->renderer);
    if (context->gpuScene)
    {
        Destruct(context->allocator, context->gpuScene);
        context->gpuScene = nullptr;
    }
    if (context->jobs)
    {
        context->jobs->Join();
        context->jobs.reset();
    }
    context->psoCache.Reset();
    context->swapchain.Reset();
    context->surface.Reset();
    context->device.Reset();
    Destruct(context->allocator, context->application);
    Destruct(context->allocator, context);
}