#include <Bindings/ImGui.hpp>

bool /* should close */ mainLoop()
{
    if (!GEditor->renderer)
        RendererSetup(GEditor, {});
    SDL_Event& event = GEditor->event;
    if (SDL_PollEvent(&event))
    {
        if (event.window.windowID != SDL_GetWindowID(GEditor->window))
            return false;
    }
    if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return true;
    if (event.type == SDL_EVENT_WINDOW_RESIZED)
    {
        UpdateSwapchain(GEditor);
        if (GEditor->renderer)
            GEditor->renderer->SetSwapchain(GEditor->swapchain);
    }
    return false;
}

constexpr int kSDLWindowFlagsVulkan = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN;
int main()
{
    CreateEditorContext(SDL_CreateWindow("Editor", 800, 600, kSDLWindowFlagsVulkan));
    while (!mainLoop()) {}
    DestroyEditorContext();
}
