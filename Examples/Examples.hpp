#pragma once
#include <RHIVulkan/Application.hpp>
#include <RenderCore/Renderer.hpp>

using namespace Foundation;
using namespace Core;
using namespace RenderCore;

constexpr RHIResourceFormat kFormatPreferenceList[] = {
    RHIResourceFormat::R8G8B8A8Unorm, RHIResourceFormat::B8G8R8A8Unrom, RHIResourceFormat::R8G8B8A8Srgb,
    RHIResourceFormat::B8G8R8A8Srgb};

constexpr RHISwapchainPresentMode kPresentModePreferenceList[] = {
    RHISwapchainPresentMode::Mailbox, RHISwapchainPresentMode::Tearing, RHISwapchainPresentMode::Fifo};

namespace details
{
    inline void CreateSwapchain(SDL_Window* window, RHIDevice* device,
                                        RHIDeviceScopedObjectHandle<RHISwapchain>& outSwap)
    {
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        LOG_RUNTIME(RenderApplication, info, "Creating swapchain ({}x{})", w, h);
        device->WaitIdle();
        if (outSwap)
            outSwap.Reset();
        auto format = Ranges::FirstOf(Views::all(kFormatPreferenceList) |
                                      Views::filter(Ranges::ContainedBy(device->GetSwapchainSupportedFormats())));
        auto present = Ranges::FirstOf(Views::all(kPresentModePreferenceList) |
                                       Views::filter(Ranges::ContainedBy(device->GetSwapchainSupportedPresentModes())));
        CHECK_MSG(format.has_value(), "No supported swapchain format found!");
        LOG_RUNTIME(RenderApplication, info, "Selected swapchain format: {}", format.value());
        CHECK_MSG(present.has_value(), "No supported presentation mode found!");
        LOG_RUNTIME(RenderApplication, info, "Selected swapchain present mode: {}", present.value());
        outSwap = device->CreateSwapchain(RHISwapchain::SwapchainDesc{
            .format = format.value(),
            .extents = RHIExtent3D{w, h, 1},
            .minBufferCount = 3,
            .presentMode = present.value(),
        });
    }
}
// [renderer, app, device, swapchain]
inline auto Examples_InitVulkan(SDL_Window* window, RendererDesc const& desc = {})
{
    auto app = Construct<VulkanApplication>(GLOBAL_ALLOC, GLOBAL_ALLOC);
    auto device = app->CreateDevice({}, window);
    RHIDeviceScopedObjectHandle<RHISwapchain> swap;
    details::CreateSwapchain(window, *device, swap);
    auto renderer = Construct<Renderer>(GLOBAL_ALLOC, desc, device, swap, GLOBAL_ALLOC);
    return std::make_tuple(renderer, app, std::move(device), std::move(swap));
}
// Polls event, possibly resizing the swapchain, and returns true if the window should close.
inline bool Examples_ShouldClose(SDL_Window* window, Renderer* renderer, RHIDeviceScopedObjectHandle<RHISwapchain>& swap, SDL_Event& event)
{
    if (!SDL_PollEvent(&event) || event.window.windowID != SDL_GetWindowID(window)) return false;
    if (event.type == SDL_EVENT_QUIT) return true;
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
inline auto Examples_DestroyVulkan(SDL_Window* window, Renderer* renderer, VulkanApplication* app)
{
    Destruct(GLOBAL_ALLOC, renderer);
    Destruct(GLOBAL_ALLOC, app);
    SDL_DestroyWindow(window);
}