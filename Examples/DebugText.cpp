#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSDebugText.hpp>
using namespace RenderUtils;
int main()
{
    SDL_Window* window = SDL_CreateWindow("DebugText Example", 800, 600, Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, swapchain] = Examples_InitVulkan(window, {});
    CSDebugTextData textData{
        .x = 16, .y = 16,
        .w = 128, .h = 16,
        .text = "Test test test."
    };
    renderer->BeginSetup();
    createCSClearBackBuffer(renderer, "Clear");
    createCSDebugTextPassBackBuffer(renderer, "Debug Text", &textData);
    renderer->EndSetup();
    while (!Examples_ShouldClose(window, renderer, swapchain))
        Examples_NewFrame(renderer);
    Examples_DestroyVulkan(window, renderer, app, device, swapchain);
}
