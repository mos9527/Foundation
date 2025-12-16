#include "Examples.hpp"
#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSDebugText.hpp>
using namespace RenderUtils;
struct UBO
{
    uint2 extent;
    float4x4 invViewProj;
    float zNear;
};
int main()
{
    SDL_Window* window = SDL_CreateWindow("Simple Path Tracer", 800, 600, Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, swapchain] = Examples_InitVulkan(window, {
        .threadCount = 0 /* ST recording */
    });
    CSDebugTextData lines[5]{};
    lines[0].x = lines[0].y = 16, lines[0].SetText("Simple Path Tracer");
    renderer->BeginSetup();
    renderer->CreatePass(
            "Trace", RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindBackbufferUAV(self, 0u);
                r->BindShader(self, RHIShaderStageBits::Compute, "main", "data/shaders/PathTracer.spv");
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(CSClearBufferData));
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                RHIExtent2D wh = r->GetSwapchainExtent();
                CSClearBufferData cdata{clearColor, wh.x, wh.y};
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, cdata);
                r->CmdDispatch(self, cmd, {cdata.w, cdata.h, 1});
            });
    createCSDebugTextPassBackBuffer(renderer, "Path Tracing", lines);
    renderer->EndSetup();
    ExampleFpsCounter fps;
    while (!Examples_ShouldClose(window, renderer, swapchain))
    {
        lines[1].x = 16, lines[1].y = 40, lines[1].SetText(fmt::format("FPS: {}", fps.Update()));
        Examples_NewFrame(renderer);
    }
    Examples_DestroyVulkan(window, renderer, app, device, swapchain);
}
