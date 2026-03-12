#include "Examples.hpp"
#include "Examples.hpp"
#include <RenderUtils/PSFullscreen.hpp>
#include <RenderUtils/CSDebugText.hpp>
using namespace RenderUtils;
int main()
{
    SDL_Window* window = SDL_CreateWindow("Hello World", 1024, 768, Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, swapchain] = Examples_InitVulkan(window, {
        .threadCount = 0 /* ST recording */
    });
    CSDebugTextData lines[5];
    lines[0].x = lines[0].y = 16, lines[0].SetText("Triangle, or Hello World in 3 vertices.");
    renderer->BeginSetup();
    renderer->CreatePass(
        "Triangle", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r) {
            r->BindBackbufferRTV(self);
            r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", Foundation::Core::PathsResolve("data/shaders/Triangle.spv"));
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", Foundation::Core::PathsResolve("data/shaders/Triangle.spv"));
            r->BindPushConstant(self, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0, sizeof(float));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd) {
            auto const& img_wh = r->GetSwapchainExtent();
            r->CmdBeginGraphics(self, cmd, img_wh);
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0, Examples_GetTime());
            cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                .SetScissor(0, 0, img_wh.x, img_wh.y)
                .Draw(3)
                .EndGraphics();
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
