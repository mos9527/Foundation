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
    auto ctx = Examples_InitVulkan(window, argc, argv, {
        .threadCount = 0 /* ST recording */
    });
    CSDebugTextData lines[5]{};
    lines[0].x = lines[0].y = 16, lines[0].SetText("Debug Text - port courtesy of https://github.com/zeux/niagara/");
    ctx.renderer->BeginSetup();
    createCSClearBackBuffer(ctx.renderer.get(), "Clear");
    createCSDebugTextPassBackBuffer(ctx.renderer.get(), "Debug Text", lines);
    ctx.renderer->EndSetup();
    ExampleFpsCounter fps;
    while (!Examples_ShouldClose(window, ctx))
    {
        lines[1].x = 16, lines[1].y = 40, lines[1].SetText(Format("FPS: {}", fps.Update()));
        Examples_NewFrame(window, ctx);
    }
    Examples_DestroyVulkan(window, ctx);
    return 0;
}
