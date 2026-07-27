// Uses a compute shader to render the fractal directly into the backbuffer!
// Shows a compact compute pass with time-varying push constants.
#include "Examples.hpp"
#include <RenderUtils/CSDebugText.hpp>
using namespace RenderUtils;
struct PushConstant
{
    float time;
    float pad;
    RHIExtent2D resolution;
};
int main(int argc, char** argv)
{
    SDL_Window* window =
        SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("Mandelbrot Compute"), 800, 600, Examples_SDLWindowFlagsVulkan);
    auto ctx = Examples_InitVulkan(window, argc, argv, {});
    CSDebugTextData lines[5]{};
    lines[0].x = lines[0].y = 16, lines[0].SetText("Mandelbrot Compute");
    ctx.renderer->BeginSetup();
    ctx.renderer->CreatePass(
        "Mandelbrot", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "csMain", Foundation::Core::PathsResolve("Data/Shaders/MandelbrotCompute.spv"));
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
