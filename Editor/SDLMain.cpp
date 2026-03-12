#include "ImGui.hpp"
#include "Paths.hpp"
extern bool EditorProcessEvent(SDL_Event*);
extern bool EditorOnFrame(FContext*);
bool /* should close */ mainLoop()
{
    SDL_Event& event = GContext->event;
    while (SDL_PollEvent(&event))
    {
        if (event.window.windowID != SDL_GetWindowID(GContext->window))
            return false;
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            return true;
        if (event.type == SDL_EVENT_WINDOW_RESIZED)
        {
            UpdateSwapchain(GContext);
            if (GContext->renderer)
                GContext->renderer->SetSwapchain(GContext->swapchain);
        }
        if (EditorProcessEvent(&event))
            return true;
    }
    EditorOnFrame(GContext);
    return false;
}

constexpr int kSDLWindowFlagsVulkan = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN;
int main(int argc, char** argv)
{
    Paths::Init(argv[0]);
    CreateContext(SDL_CreateWindow("Foundation Editor", 1920, 1080, kSDLWindowFlagsVulkan));
    ImGui_ImplFoundation_SetupContextWithDefaultStyles();
    ImGui_ImplFoundation_Init(GContext->device.Get(), GContext->window);
    GContext->args = Span(argv, argv + argc);
    while (!mainLoop()) {}
    LOG(SDLMain, LogInfo, "Quitting...");
    ImGui_ImplFoundation_Shutdown();
    DestroyContext();
}
