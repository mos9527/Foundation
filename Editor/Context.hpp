#pragma once
#include <Core/AllocatorStack.hpp>
#include <Core/ThreadPool.hpp>
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
    struct WindowHDRState
    {
        bool propertiesAvailable{false};
        bool enabled{false};
        float sdrWhiteLevel{1.0f};
        float sdrWhiteNits{80.0f};
        float headroom{1.0f};
        float peakNits{80.0f};
    } windowHDR;

    RHIApplication* application{};
    RHIApplicationScopedHandle<RHIDevice> device;
    RHIDeviceScopedHandle<RHISwapchain> swapchain;
    RHIDeviceScopedHandle<RHIPipelineStateCache> psoCache;
    String psoCachePath;

    GPUScene* gpuScene{};
    Renderer* renderer{};

    // Persistent worker pool for main-thread fork-join work (e.g. CPU skinning via ParallelFor).
    UniquePtr<ThreadPool> jobs;

    SDL_Event event;

    bool enableHDR{false};
};

extern FContext* GContext;

extern bool UpdateWindowHDRState(FContext* context);
extern void UpdateSwapchain(FContext* context);
extern void ResetEditorFrameScratch(FContext* context);
extern void DestroyEditorRenderer(FContext* context = nullptr);
extern FContext* CreateContext(SDL_Window* window, Allocator* allocator = GLOBAL_ALLOC, RHIDevice::DeviceDesc const& deviceDesc = {});
extern void DestroyContext(FContext* context = nullptr /* global */);
