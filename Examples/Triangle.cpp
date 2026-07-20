// Hello-world graphics example that renders a single animated triangle.
// Useful as the smallest windowed Renderer + Vulkan setup path.
#include "Examples.hpp"
#include "Examples.hpp"
#include <RenderUtils/PSFullscreen.hpp>
#include <RenderUtils/CSDebugText.hpp>
using namespace RenderUtils;
int main(int argc, char** argv)
{
    SDL_Window* window =
        SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("Hello World"), 1024, 768, Examples_SDLWindowFlagsVulkan);
    auto ctx = Examples_InitVulkan(window, argc, argv, {
        .threadCount = 0 /* ST recording */
    });
    CSDebugTextData lines[5]{};
    lines[0].x = lines[0].y = 16, lines[0].SetText("Triangle, or Hello World in 3 vertices.");
    ctx.renderer->BeginSetup();
    ctx.renderer->CreatePass(
        "Triangle", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r) {
            r->BindBackbufferRTV(self);
            r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", Foundation::Core::PathsResolve("Data/Shaders/Triangle.spv"));
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", Foundation::Core::PathsResolve("Data/Shaders/Triangle.spv"));
            r->BindPushConstant(self, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0, sizeof(float));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd) {
            auto const& img_wh = r->GetSwapchainExtent();
            r->CmdBeginGraphics(self, cmd, img_wh, {{{RHIAttachmentLoadOp::Clear}}});
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0, Examples_GetTime());
            cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                .SetScissor(0, 0, img_wh.x, img_wh.y)
                .Draw(3)
                .EndGraphics();
        }
    );
    createCSDebugTextPassBackBuffer(ctx.renderer.get(), "Debug Text", lines);
    ctx.renderer->EndSetup();
    ExampleFpsCounter fps;
    while (!Examples_ShouldClose(window, ctx))
    {
        lines[1].x = 16, lines[1].y = 40, lines[1].SetText(fmt::format("FPS: {}", fps.Update()));
        Examples_NewFrame(window, ctx);
    }
    Examples_DestroyVulkan(window, ctx);
}
