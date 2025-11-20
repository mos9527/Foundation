#include "Examples.hpp"
#include <RenderUtils/CSDebugText.hpp>
using namespace RenderUtils;
struct PushConstant
{
    float time;
    float pad;
    RHIExtent2D resolution;
};
int main()
{
    SDL_Window* window = SDL_CreateWindow("Mandelbrot Compute", 800, 600, Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, swapchain] = Examples_InitVulkan(window, {});
    CSDebugTextData lines[5]{};
    lines[0].x = lines[0].y = 16, lines[0].SetText("Mandelbrot Compute");
    renderer->BeginSetup();
    renderer->CreatePass(
        "Mandelbrot", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "csMain", "data/shaders/MandelbrotCompute.spv");
            r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(PushConstant));
            r->BindBackbufferUAV(self, 0);
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            auto image_wh = r->GetSwapchainExtent3D();
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0,
                                  PushConstant{.time = Examples_GetTime(), .resolution = image_wh});
            r->CmdDispatch(self, cmd, image_wh);
        });
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
