#pragma once
#include <Core/AllocatorStack.hpp>
#include <RenderCore/Renderer.hpp>
#include <SDL3/SDL.h>

using namespace Foundation;
using namespace RHI;
using namespace RenderCore;
using namespace Core;
class GPUScene;
static constexpr size_t kEditorFrameScratchSize = 4 << 20;

struct FContext
{
    Span<const char*> files;

    Allocator* allocator{};
    UniquePtr<ScopedArena> editorFrameArena;
    UniquePtr<AllocatorStack> editorFrameScratch;

    SDL_Window* window{};

    RHIApplication* application{};
    RHIApplicationScopedHandle<RHIDevice> device;
    RHIDeviceScopedHandle<RHISwapchain> swapchain;
    RHIDeviceScopedHandle<RHIPipelineStateCache> psoCache;

    GPUScene* gpuScene{};
    Renderer* renderer{};

    SDL_Event event;

    bool enableHDR{false};
};

extern FContext* GContext;

extern void UpdateSwapchain(FContext* context);
extern void ResetEditorFrameScratch(FContext* context);

extern FContext* CreateContext(SDL_Window* window, Allocator* allocator = GLOBAL_ALLOC, RHIDevice::DeviceDesc const& deviceDesc = {});
extern void DestroyContext(FContext* context = nullptr /* global */);
