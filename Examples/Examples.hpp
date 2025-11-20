#pragma once
#include <RHIVulkan/Application.hpp>
#include <RenderCore/Renderer.hpp>
#include <Math/Math.hpp>
using namespace Foundation;
using namespace Core;
using namespace Math;
using namespace RenderCore;

constexpr RHIResourceFormat kFormatPreferenceList[] = {
    RHIResourceFormat::R8G8B8A8Unorm, RHIResourceFormat::B8G8R8A8Unrom, RHIResourceFormat::R8G8B8A8Srgb,
    RHIResourceFormat::B8G8R8A8Srgb};

constexpr RHISwapchainPresentMode kPresentModePreferenceList[] = {
    RHISwapchainPresentMode::Mailbox, RHISwapchainPresentMode::Tearing, RHISwapchainPresentMode::Fifo};

namespace details
{
    inline void CreateSwapchain(SDL_Window* window, RHIDevice* device,
                                        RHIDeviceUniqueRef<RHISwapchain>& outSwap)
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
        LOG(RenderApplication, LogDebug, "Selected swapchain format: {}", format.value());
        CHECK_MSG(present.has_value(), "No supported presentation mode found!");
        LOG(RenderApplication, LogDebug, "Selected swapchain present mode: {}", present.value());
        outSwap = device->CreateSwapchain(RHISwapchain::SwapchainDesc{
            .format = format.value(),
            .extents = RHIExtent3D{w, h, 1},
            .minBufferCount = 3,
            .presentMode = present.value(),
        });
    }
}
// [renderer, app, device, swapchain]
constexpr int Examples_SDLWindowFlagsVulkan = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN;
inline auto Examples_InitVulkan(SDL_Window* window, RendererDesc const& desc = {})
{
    auto app = Construct<VulkanApplication>(GLOBAL_ALLOC, GLOBAL_ALLOC);
    auto device = app->CreateDevice({}, window);
    RHIDeviceUniqueRef<RHISwapchain> swap;
    details::CreateSwapchain(window, *device, swap);
    auto renderer = Construct<Renderer>(GLOBAL_ALLOC, desc, device, swap, GLOBAL_ALLOC);
    return std::make_tuple(renderer, app, std::move(device), std::move(swap));
}
// Polls event, possibly resizing the swapchain, and returns true if the window should close.
inline bool Examples_ShouldClose(SDL_Window* window, Renderer* renderer, RHIDeviceUniqueRef<RHISwapchain>& swap, SDL_Event* outEvent = nullptr)
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
    if (event.type == SDL_EVENT_WINDOW_RESIZED)
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
inline auto Examples_DestroyVulkan(SDL_Window* window, Renderer* renderer, VulkanApplication* app, RHIApplicationUniqueRef<RHIDevice>& device, RHIDeviceUniqueRef<RHISwapchain>& swapchain)
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
        mat4 proj = infinitePerspectiveLHReverseZ(fovY, aspect, zNear);
        quat rot = angleAxis(yaw, vec3(1, 0, 0)) * angleAxis(pitch, vec3(0, 1, 0));
        vec3 dir = rot * vec3(0, 0, 1);
        mat4 view = viewMatrixLHReverseZ(center + radius * dir, rot);
        mat4 viewProj = proj * view;
        return viewProj;
    }
};