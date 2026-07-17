// Example showing how to use CSDebugText to get the absolute minimum up and running - with something to display.
// You can copy-paste this into your own application to get started.
#include "Examples.hpp"
#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSDebugText.hpp>
using namespace RenderUtils;
int main(int argc, char** argv)
{
    SDL_Window* window =
        SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("DebugText Example"), 800, 600, Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, surface, swapchain, presenter] = Examples_InitVulkan(window, argc, argv, {
        .threadCount = 0 /* ST recording */
    });
    CSDebugTextData lines[5]{};
    lines[0].x = lines[0].y = 16, lines[0].SetText("Debug Text - port courtesy of https://github.com/zeux/niagara/");
    renderer->BeginSetup();
    createCSClearBackBuffer(renderer, "Clear");
    createCSDebugTextPassBackBuffer(renderer, "Debug Text", lines);
    renderer->EndSetup();
    ExampleFpsCounter fps;
    while (!Examples_ShouldClose(window, renderer, surface, swapchain))
    {
        lines[1].x = 16, lines[1].y = 40, lines[1].SetText(fmt::format("FPS: {}", fps.Update()));
        Examples_NewFrame(window, renderer, presenter, surface, swapchain);
    }
    Examples_DestroyVulkan(window, renderer, app, device, surface, swapchain);
}
