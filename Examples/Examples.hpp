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
inline auto Examples_InitVulkan(SDL_Window* window, int argc, char** argv, RendererDesc const& desc = {})
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
    if (cmdl[{"-l", "--list-gpus"}])
    {
        auto* app = Construct<VulkanApplication>(GLOBAL_ALLOC, GLOBAL_ALLOC);
        for (auto const& d : app->EnumerateDevices())
            fmt::println("[{}] {}", d.id, d.name);
        Destruct(GLOBAL_ALLOC, app);
        std::exit(0);
    }
    int gpuId = 0;
    cmdl({"-g", "--gpu"}, 0) >> gpuId;
    PathsInitFromDir(SDL_GetBasePath());
    auto app = Construct<VulkanApplication>(GLOBAL_ALLOC, GLOBAL_ALLOC);
    auto device = app->CreateDevice({.id = static_cast<uint32_t>(gpuId)}, window);
    RHIDeviceScopedHandle<RHISwapchain> swap;
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
    SDL_DestroyWindow(window);
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


struct ExamplesArcballCamera
{
    static constexpr char kControlsText[] = "Mouse Left: Rotate | Mouse Right: Pan | Mouse Wheel: Zoom";

    float3 center;
    float radius;
    float pitch, yaw;
    float zNear, fovY, aspect;
    mat4 view, proj;
    mat4 Update(SDL_Event const& event)
    {
        if (event.type == SDL_EVENT_MOUSE_MOTION)
        {
            if (event.motion.state & SDL_BUTTON_LMASK)
            {
                pitch -= event.motion.xrel * 1e-2f;
                yaw -= event.motion.yrel * 1e-2f;
            }
            if (event.motion.state & SDL_BUTTON_RMASK)
            {
                quat rot = angleAxis(yaw, vec3(1, 0, 0)) * angleAxis(pitch, vec3(0, 1, 0));
                vec3 right = rot * vec3(1, 0, 0);
                vec3 up = rot * vec3(0, 1, 0);
                center -= right * (event.motion.xrel * radius * 1e-3f);
                center += up * (event.motion.yrel * radius * 1e-3f);
            }
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL)
        {
            radius -= event.wheel.y * radius * 1e-1f;
            radius = radius < 1e-3f ? 1e-3f : radius;
        }
        // ---
        proj = infinitePerspectiveRHReverseZ(fovY, aspect, zNear);
        quat rot = angleAxis(yaw, vec3(1, 0, 0)) * angleAxis(pitch, vec3(0, 1, 0));
        vec3 dir = rot * vec3(0, 0, 1);
        view = viewMatrixRHReverseZ(center + radius * dir, rot);
        mat4 viewProj = proj * view;
        return viewProj;
    }
};
