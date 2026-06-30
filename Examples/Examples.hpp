#pragma once
#include <Core/Paths.hpp>
#include <RHIVulkan/Application.hpp>
#include <RenderCore/Renderer.hpp>
#include <Renderer/GPUScene.hpp>
#include <Renderer/Postprocess.hpp>
#include <Renderer/Texture.hpp>
#include <Math/Math.hpp>
#include <Math/ModelViewProjection.hpp>
#include <RenderUtils/PSFullscreen.hpp>
#include <argh.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stb_image_write.h>
#include <SDL3/SDL_main.h>
#include <vector>
using namespace Foundation;
using namespace Core;
using namespace Math;
using namespace RenderCore;

constexpr RHISurfaceFormat kFormatPreferenceList[] = {
    {RHIResourceFormat::R8G8B8A8Unorm, RHIColorSpace::SrgbNonLinear},
    {RHIResourceFormat::B8G8R8A8Unrom, RHIColorSpace::SrgbNonLinear},
};

constexpr RHISwapchainPresentMode kPresentModePreferenceList[] = {
    RHISwapchainPresentMode::Mailbox, RHISwapchainPresentMode::Tearing, RHISwapchainPresentMode::Fifo};

namespace details
{
    inline String PipelineCachePathForDevice(RHIDevice const& device)
    {
        auto key = device.GetPipelineCacheKey();
        return PathsResolve(fmt::format("Cache/PipelineCache/Vulkan/pso-cache-{:016x}-{:016x}.bin", key.high, key.low));
    }

    inline Vector<unsigned char> LoadPipelineCacheBytes(StringView path, Allocator* allocator)
    {
        Vector<unsigned char> data(allocator);
        if (!std::filesystem::exists(path.data()))
            return data;

        std::ifstream file(path.data(), std::ios::binary | std::ios::ate);
        if (!file)
        {
            LOG(Examples, LogWarn, "Failed to open pipeline cache file for reading: {}", path);
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
            LOG(Examples, LogWarn, "Failed to read pipeline cache file: {}", path);
            data.clear();
        }
        return data;
    }

    inline void ClearStalePipelineCaches(StringView activePath)
    {
        std::filesystem::path cacheDir = std::filesystem::path(activePath).parent_path();
        if (!std::filesystem::exists(cacheDir))
            return;

        std::filesystem::path active(activePath);
        for (auto const& entry : std::filesystem::directory_iterator(cacheDir))
        {
            if (!entry.is_regular_file() || entry.path() == active)
                continue;

            auto filename = entry.path().filename().string();
            if (!filename.starts_with("pso-cache-") || !filename.ends_with(".bin"))
                continue;

            LOG(Examples, LogInfo, "Clearing stale pipeline cache: {}", entry.path().string());
            std::error_code ec;
            std::filesystem::remove(entry.path(), ec);
        }
    }

    inline void SavePipelineCache(RHIPipelineStateCache const& cache, StringView path, Allocator* allocator)
    {
        size_t size = cache.GetSerializedDataSize();
        if (size == 0)
            return;

        Vector<unsigned char> data(size, allocator);
        size_t written = cache.WriteSerializedData(data);
        if (written == 0)
            return;

        data.resize(written);
        std::filesystem::path fsPath(path);
        std::filesystem::create_directories(fsPath.parent_path());
        std::ofstream file(fsPath, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            LOG(Examples, LogWarn, "Failed to open pipeline cache file for writing: {}", path);
            return;
        }

        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!file)
        {
            LOG(Examples, LogWarn, "Failed to write pipeline cache file: {}", path);
        }
        else
        {
            LOG(Examples, LogInfo, "Saved pipeline cache: {} ({} bytes)", path, data.size());
        }
    }

    struct PipelineCacheContext
    {
        Renderer* renderer{};
        RHIDeviceScopedHandle<RHIPipelineStateCache> cache;
        String path;
    };

    inline std::vector<PipelineCacheContext>& PipelineCacheContexts()
    {
        static std::vector<PipelineCacheContext> contexts;
        return contexts;
    }

    inline void CreateSwapchain(SDL_Window* window, RHIDevice* device,
                                        RHIDeviceScopedHandle<RHISwapchain>& outSwap)
    {
        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        LOG(RenderApplication, LogDebug, "Creating swapchain ({}x{})", w, h);
        device->WaitIdle();
        if (outSwap)
            outSwap.Reset();
        auto format = Ranges::FirstOf(Views::all(kFormatPreferenceList) |
                                      Views::filter(Ranges::ContainedBy(device->GetSwapchainSupportedFormats())));
        auto present = Ranges::FirstOf(Views::all(kPresentModePreferenceList) |
                                       Views::filter(Ranges::ContainedBy(device->GetSwapchainSupportedPresentModes())));
        CHECK_MSG(format.has_value(), "No supported swapchain format found!");
        LOG(RenderApplication, LogDebug, "Selected swapchain format: {} with color space: {}", format.value().format, format.value().colorSpace);
        CHECK_MSG(present.has_value(), "No supported presentation mode found!");
        LOG(RenderApplication, LogDebug, "Selected swapchain present mode: {}", present.value());
        outSwap = device->CreateSwapchain(RHISwapchain::SwapchainDesc{
            .format = format.value().format,
            .colorSpace = format.value().colorSpace,
            .extents = RHIExtent3D{w, h, 1},
            .minBufferCount = 3,
            .presentMode = present.value(),
        });
    }
}
// [renderer, app, device, swapchain]
constexpr int Examples_SDLWindowFlagsVulkan = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN;
// Common command-line options shared by all examples (mirrors Editor::SDLMain):
//   -h, --help        Show usage and exit
//   -g, --gpu <id>    Select GPU device index
//   -l, --list-gpus   List available GPU devices and exit
//
#if defined(__ANDROID__)
inline void Examples_ExtractAssets(const char* internalDir) {
    const char* assetsToExtract[] = {
        "Data/Shaders/Triangle.spv",
        "Data/Shaders/SDF2D.spv",
        "Data/Shaders/MandelbrotCompute.spv",
        "Data/Shaders/CSDebugText.spv",
        "Data/Shaders/VSFullscreen.spv",
        "Data/Shaders/PSCopy.spv"
    };
    for (const char* asset : assetsToExtract) {
        size_t size;
        void* data = SDL_LoadFile(asset, &size);
        if (data) {
            std::string outPath = std::string(internalDir) + "/" + asset;
            std::filesystem::path fsPath(outPath);
            std::filesystem::create_directories(fsPath.parent_path());
            std::ofstream file(outPath, std::ios::binary | std::ios::trunc);
            if (file) {
                file.write(reinterpret_cast<const char*>(data), size);
            }
            SDL_free(data);
        }
    }
}
#endif

// Pass desc.present = false (with desc.renderExtent set) to initialize headlessly:
// no SDL window or Vulkan WSI is required, no swapchain is created, and the returned
// swapchain handle is invalid. This works on software backends (e.g. Lavapipe) without
// surface extensions. In that case `window` is ignored and may be nullptr.
inline auto Examples_InitVulkan(SDL_Window* window, int argc, char** argv, RendererDesc desc = {})
{
    argh::parser cmdl(argc, argv, argh::parser::PREFER_PARAM_FOR_UNREG_OPTION);
    if (cmdl[{"-h", "--help"}])
    {
        fmt::println("Usage: {} [options]", argv[0]);
        fmt::println("Options:");
        fmt::println("\t-h, --help\t\tShow this help message");
        fmt::println("\t-g, --gpu <id>\t\tSpecify GPU device index");
        fmt::println("\t-l, --list-gpus\t\tList available GPU devices");
        std::exit(0);
    }
    const bool headless = !desc.present;
    if (cmdl[{"-l", "--list-gpus"}])
    {
        auto* app = Construct<VulkanApplication>(GLOBAL_ALLOC, GLOBAL_ALLOC, headless);
        for (auto const& d : app->EnumerateDevices())
            fmt::println("[{}] {}", d.id, d.name);
        Destruct(GLOBAL_ALLOC, app);
        std::exit(0);
    }
    int gpuId = 0;
    cmdl({"-g", "--gpu"}, 0) >> gpuId;
    if (headless)
        PathsInit(argv[0]);
    else
    {
#if defined(__ANDROID__)
        const char* prefPath = SDL_GetPrefPath("foundation", "examples");
        Examples_ExtractAssets(prefPath);
        PathsInitFromDir(prefPath);
        SDL_free((void*)prefPath);
#else
        PathsInitFromDir(SDL_GetBasePath());
#endif
    }
    auto app = Construct<VulkanApplication>(GLOBAL_ALLOC, GLOBAL_ALLOC, headless);
    auto device = headless ? app->CreateDevice({.id = static_cast<uint32_t>(gpuId)})
                           : app->CreateDevice({.id = static_cast<uint32_t>(gpuId)}, window);
    RHIDeviceScopedHandle<RHISwapchain> swap;
    if (!headless)
        details::CreateSwapchain(window, *device, swap);
    RHIDeviceScopedHandle<RHIPipelineStateCache> psoCache;
    String psoCachePath;
    if (!desc.pipelineCache)
    {
        psoCachePath = details::PipelineCachePathForDevice(*device.Get());
        details::ClearStalePipelineCaches(psoCachePath);
        auto psoCacheBytes = details::LoadPipelineCacheBytes(psoCachePath, GLOBAL_ALLOC);
        psoCache = device->CreatePipelineCache({
            .initialData = Span<const unsigned char>(psoCacheBytes.data(), psoCacheBytes.size())
        });
        LOG(Examples, LogInfo, "Pipeline cache {}: {} ({} bytes)",
            psoCache->GetImportStatus(),
            psoCachePath,
            psoCacheBytes.size());
        desc.pipelineCache = psoCache.Get();
    }
    auto renderer = Construct<Renderer>(GLOBAL_ALLOC, desc, device, swap, GLOBAL_ALLOC);
    if (psoCache)
    {
        details::PipelineCacheContexts().push_back({
            .renderer = renderer,
            .cache = std::move(psoCache),
            .path = std::move(psoCachePath),
        });
    }
    return std::make_tuple(renderer, app, std::move(device), std::move(swap));
}
// Polls event, possibly resizing the swapchain, and returns true if the window should close.
inline bool Examples_ShouldClose(SDL_Window* window, Renderer* renderer, RHIDeviceScopedHandle<RHISwapchain>& swap, SDL_Event* outEvent = nullptr)
{
    SDL_Event event{};
    bool any = SDL_PollEvent(&event);
    if (outEvent)
        *outEvent = event;
    if (!any)
        return false;
    if (event.window.windowID != SDL_GetWindowID(window))
        return false;
    if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return true;
    // Resize swapchain if necessary
    if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_MAXIMIZED || event.type == SDL_EVENT_WINDOW_RESTORED)
    {
        details::CreateSwapchain(window, swap.mFactory,swap);
        renderer->SetSwapchain(swap);
    }
    return false;
}
inline void Examples_NewFrame(Renderer* renderer)
{
    renderer->BeginExecute();
    renderer->ExecuteFrame();
    renderer->EndExecute();
}
inline auto Examples_DestroyVulkan(SDL_Window* window, Renderer* renderer, VulkanApplication* app, RHIApplicationScopedHandle<RHIDevice>& device, RHIDeviceScopedHandle<RHISwapchain>& swapchain)
{
    if (device)
        device->WaitIdle();

    auto& psoCacheContexts = details::PipelineCacheContexts();
    auto psoCacheIt = std::ranges::find_if(psoCacheContexts,
        [renderer](details::PipelineCacheContext const& context)
        {
            return context.renderer == renderer;
        });
    if (psoCacheIt != psoCacheContexts.end())
    {
        details::SavePipelineCache(*psoCacheIt->cache.Get(), psoCacheIt->path, GLOBAL_ALLOC);
    }

    Destruct(GLOBAL_ALLOC, renderer);
    if (psoCacheIt != psoCacheContexts.end())
        psoCacheContexts.erase(psoCacheIt);
    swapchain.Reset();
    device.Reset();
    Destruct(GLOBAL_ALLOC, app);
    if (window)
        SDL_DestroyWindow(window);
}

// Writes an 8-bit-per-channel image (e.g. from a readback buffer) to a PNG at `path`,
// then opens it with the OS default viewer via SDL_OpenURL. `strideBytes` defaults to
// width * channels when zero. Link ThirdParty_STB in targets that call this.
//   channels: 1 (grey), 2 (grey+alpha), 3 (RGB), 4 (RGBA)
inline void Examples_DumpAndOpenImage(StringView path, RHIExtent2D extent, void const* data,
                                      int channels = 4, int strideBytes = 0)
{
    const auto outPath = std::filesystem::absolute(std::string(path.data(), path.size())).string();
    const int stride = strideBytes ? strideBytes : static_cast<int>(extent.x) * channels;
    if (!stbi_write_png(outPath.c_str(), static_cast<int>(extent.x), static_cast<int>(extent.y),
                        channels, data, stride))
    {
        LOG(Examples, LogError, "stbi_write_png failed for '{}'", outPath);
        return;
    }
    LOG(Examples, LogInfo, "Wrote '{}'", outPath);
    if (!SDL_OpenURL(outPath.c_str()))
        LOG(Examples, LogWarn, "SDL_OpenURL failed: {}", SDL_GetError());
}

// Simplified example tonemap / postprocess pass.
//   Raster: samples `outputs.diffuse` as-is.
//   PT:     samples `outputs.diffuse + outputs.specular`.
//   Output: clamped to [0,1] then gamma-corrected (1/2.2) into an explicit LDR
//           PostprocessBuffer RTV (R8G8B8A8Unorm). When the renderer presents, that
//           RTV is blitted to the backbuffer so windowed examples display it; the RTV
//           handle is returned so headless examples can read it back. No view LUTs,
//           exposure, or debug-AOV branches - this is the minimal display path.
inline ResourceHandle Examples_InsertBasicTonemapPasses(Renderer* renderer, RendererOutputs const& outputs,
                                                        bool isPathTracer)
{
    CHECK_MSG(outputs.diffuse != kInvalidHandle, "Basic tonemap pass missing diffuse output");
    RHIExtent2D extent = outputs.extent;
    if (extent.x == 0u || extent.y == 0u)
        extent = renderer->GetRenderExtent();
    const uint32_t w = extent.x;
    const uint32_t h = extent.y;
    constexpr RHIResourceFormat kOutputFormat = RHIResourceFormat::R8G8B8A8Unorm;

    auto postprocess = renderer->CreateResource(
        "Basic Postprocess",
        RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget |
                                  RHITextureUsageBits::SampledImage |
                                  RHITextureUsageBits::TransferSource,
                       .extent = {w, h, 1},
                       .format = kOutputFormat});

    using namespace RenderUtils;
    const char* shader = isPathTracer ? "Data/Shaders/PostprocessBasicPT.spv"
                                      : "Data/Shaders/PostprocessBasic.spv";
    createPSFullscreenPassRTV(
        renderer, "Basic Tonemap", postprocess,
        RHITextureViewDesc{.format = kOutputFormat, .range = RHITextureSubresourceRange::Create()},
        {w, h},
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", PathsResolve(shader));
            r->BindTextureSRV(self, outputs.diffuse, "diffuseTex", RHIPipelineStageBits::FragmentShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            if (isPathTracer)
            {
                const ResourceHandle specular =
                    outputs.specular != kInvalidHandle ? outputs.specular : outputs.diffuse;
                r->BindTextureSRV(self, specular, "specularTex", RHIPipelineStageBits::FragmentShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                     .range = RHITextureSubresourceRange::Create()});
            }
        },
        [](PassHandle, Renderer*, RHICommandList*) {});

    if (renderer->IsPresentEnabled())
    {
        const auto sampler = renderer->CreateSampler({});
        createPSBackbufferBlitPass(renderer, "Basic Tonemap Blit", sampler, postprocess, kOutputFormat);
    }
    return postprocess;
}

inline float Examples_GetTime()
{
    return static_cast<float>(SDL_GetTicks() / 1e3);
}


struct ExampleFpsCounter
{
    size_t lastTick{};
    size_t frames{};
    float fps{};
    float Update()
    {
        size_t now = SDL_GetTicksNS();
        size_t delta = now - lastTick;
        if (delta >= 1e9)
        {
            fps = 1e9 * (static_cast<float>(frames) / delta);
            frames = 0;
            lastTick = now;
        }
        frames++;
        return fps;
    }
};
