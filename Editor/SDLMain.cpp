extern bool EditorOnFrame(FContext*);
bool /* should close */ mainLoop()
{
    SDL_Event& event = GContext->event;
    if (SDL_PollEvent(&event))
    {
        if (event.window.windowID != SDL_GetWindowID(GContext->window))
            return false;
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return true;
        if (event.type == SDL_EVENT_WINDOW_RESIZED)
        {
            UpdateSwapchain(GContext);
            if (GContext->renderer)
                GContext->renderer->SetSwapchain(GContext->swapchain);
        }
    } else
    {
        event = {};
    }
    if (EditorOnFrame(GContext)) return true;
    return false;
}

constexpr int kSDLWindowFlagsVulkan = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN;
int main()
{
    CreateContext(SDL_CreateWindow("Editor", 800, 600, kSDLWindowFlagsVulkan));
    while (!mainLoop()) {}
    DestroyContext();
}
