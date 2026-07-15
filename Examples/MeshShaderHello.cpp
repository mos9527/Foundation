// Smallest mesh-shader pipeline sample: task + mesh + fragment shaders draw one object.
// Useful for validating mesh shader support and dispatch wiring.
#include "Examples.hpp"
#include <RenderUtils/CSDebugText.hpp>
using namespace RenderUtils;
int main(int argc, char** argv)
{
    SDL_Window* window = SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("Mesh Shader Hello World"), 800, 600,
                                          Examples_SDLWindowFlagsVulkan);
    CSDebugTextData data{ .x = 16, .y = 16 };
    auto [renderer, app, device, swapchain] = Examples_InitVulkan(window, argc, argv, {});
    CHECK_MSG(device->GetCapabilities().meshShaders, "Mesh Shader support required, but is unavailable");
    renderer->BeginSetup();
    renderer->CreatePass(
        "Mesh Task", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBackbufferRTV(self);
            r->BindShader(self, RHIShaderStageBits::Task, "taskMain", Foundation::Core::PathsResolve("Data/Shaders/MeshShaderHelloTask.spv"));
            r->BindShader(self, RHIShaderStageBits::Mesh, "meshMain", Foundation::Core::PathsResolve("Data/Shaders/MeshShaderHelloMesh.spv"));
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", Foundation::Core::PathsResolve("Data/Shaders/MeshShaderHelloFrag.spv"));
            r->BindPushConstant(self, RHIShaderStageBits::Task, 0, sizeof(float));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            auto const& img_wh = r->GetSwapchainExtent();
            r->CmdBeginGraphics(self, cmd, img_wh, {{{RHIAttachmentLoadOp::Clear}}});
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Task, 0, Examples_GetTime());
            cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                .SetScissor(0, 0, img_wh.x, img_wh.y)
                .DrawMeshTasks(1, 1, 1)
                .EndGraphics();
        });
    createCSDebugTextPassBackBuffer(renderer, "Debug Text", {{data}});
    renderer->EndSetup();
    ExampleFpsCounter fps;
    while (!Examples_ShouldClose(window, renderer, swapchain))
    {
        data.SetText(fmt::format("MeshShader Hello World FPS: {}", fps.Update()));
        Examples_NewFrame(window, renderer, swapchain);
    }
    Examples_DestroyVulkan(window, renderer, app, device, swapchain);
}
