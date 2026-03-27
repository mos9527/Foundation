#pragma once
#include <RenderCore/Renderer.hpp>
#include <SDL3/SDL.h>

using namespace Foundation;
using namespace RHI;
using namespace RenderCore;
using namespace Core;
class GPUScene;
struct FContext
{
    Span<const char*> files;

    Allocator* allocator{};

    SDL_Window* window{};

    RHIApplication* application{};
    RHIApplicationScopedHandle<RHIDevice> device;
    RHIDeviceScopedHandle<RHISwapchain> swapchain;
    RHIDeviceScopedHandle<RHIPipelineStateCache> psoCache;

    GPUScene* gpuScene{};
    Renderer* renderer{};

    SDL_Event event;
};

extern FContext* GContext;

extern void UpdateSwapchain(FContext* context);

extern FContext* CreateContext(SDL_Window* window, Allocator* allocator = GLOBAL_ALLOC, RHIDevice::DeviceDesc const& deviceDesc = {});
extern void DestroyContext(FContext* context = nullptr /* global */);
