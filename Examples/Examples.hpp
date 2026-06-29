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
#include <stb_image_write.h>
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
        PathsInitFromDir(SDL_GetBasePath());
    auto app = Construct<VulkanApplication>(GLOBAL_ALLOC, GLOBAL_ALLOC, headless);
    auto device = headless ? app->CreateDevice({.id = static_cast<uint32_t>(gpuId)})
                           : app->CreateDevice({.id = static_cast<uint32_t>(gpuId)}, window);
    RHIDeviceScopedHandle<RHISwapchain> swap;
    if (!headless)
        details::CreateSwapchain(window, *device, swap);
    auto renderer = Construct<Renderer>(GLOBAL_ALLOC, desc, device, swap, GLOBAL_ALLOC);
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
    Destruct(GLOBAL_ALLOC, renderer);
    if (device)
        device->WaitIdle();
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
