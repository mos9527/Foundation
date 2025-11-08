#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSDebugText.hpp>
using namespace RenderUtils;
int main()
{
    SDL_Window* window = SDL_CreateWindow("DebugText Example", 800, 600, Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, swapchain] = Examples_InitVulkan(window, {
        .threads = 0 /* ST recording */
    });
    CSDebugTextData lines[5];
    lines[0].x = lines[0].y = 16, fmt::format_to(lines[0].text.ch, "Debug Text - port courtesy of https://github.com/zeux/niagara/");
    renderer->BeginSetup();
    createCSClearBackBuffer(renderer, "Clear");
    createCSDebugTextPassBackBuffer(renderer, "Debug Text", lines);
    renderer->EndSetup();
    ExampleFpsCounter fps;
    while (!Examples_ShouldClose(window, renderer, swapchain))
    {
        lines[1].x = 16, lines[1].y = 40, fmt::format_to(lines[1].text.ch, "FPS: {}", fps.Update());
        Examples_NewFrame(renderer);
    }
    Examples_DestroyVulkan(window, renderer, app, device, swapchain);
}
