#pragma once
#include <RenderCore/Renderer.hpp>
#include <SDL3/SDL.h>

using namespace Foundation;
using namespace RHI;
using namespace RenderCore;
using namespace Core;
struct FEditorContext
{
    Allocator* allocator;

    SDL_Window* window;
    UniquePtr<RHIApplication> application;

    RHIApplicationUniqueRef<RHIDevice> device;
    RHIDeviceUniqueRef<RHISwapchain> swapchain;

    UniquePtr<Renderer> renderer;

    SDL_Event event;
};

extern FEditorContext* GEditor;

extern void UpdateSwapchain(FEditorContext* context);

extern FEditorContext* CreateEditorContext(SDL_Window* window, Allocator* allocator = GLOBAL_ALLOC);
extern void DestroyEditorContext(FEditorContext* context = nullptr /* global */);
