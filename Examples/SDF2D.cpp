#include "Examples.hpp"
#include <RenderUtils/PSFullscreen.hpp>
#include <RenderUtils/CSDebugText.hpp>
using namespace RenderUtils;
int main(int argc, char** argv)
{
    SDL_Window* window = SDL_CreateWindow("SDF2D Example", 1024, 768, Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, swapchain] = Examples_InitVulkan(window, argc, argv, {
        .threadCount = 0 /* ST recording */
    });
    CSDebugTextData lines[5];
    lines[0].x = lines[0].y = 16, lines[0].SetText("SDF2D - port courtesy of https://iquilezles.org/articles/distfunctions2d/");
    renderer->BeginSetup();
    createPSFullscreenPass(
        renderer, "SDF2D",
        [=](PassHandle self, Renderer* r) {
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", Foundation::Core::PathsResolve("Data/Shaders/SDF2D.spv"));
            r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(float));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd) {
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, Examples_GetTime());
        }
    );
    createCSDebugTextPassBackBuffer(renderer, "Debug Text", lines);
    renderer->EndSetup();
    ExampleFpsCounter fps;
    while (!Examples_ShouldClose(window, renderer, swapchain))
    {
        lines[1].x = 16, lines[1].y = 40, lines[1].SetText(fmt::format("FPS: {}", fps.Update()));
        Examples_NewFrame(renderer);
    }
    Examples_DestroyVulkan(window, renderer, app, device, swapchain);
}
