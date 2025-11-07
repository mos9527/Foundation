#include "Examples.hpp"

int main()
{
    SDL_Window* window = SDL_CreateWindow("Mesh Shader Example", 800, 600, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    auto [renderer, app, device, swapchain] = Examples_InitVulkan(window, RendererDesc{});
    renderer->BeginSetup();
    renderer->CreatePass(
        "Mesh Task", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBackbufferRTV(self);
            r->BindShader(self, RHIShaderStageBits::Task, "taskMain", "data/shaders/MeshShaderTask.spv");
            r->BindShader(self, RHIShaderStageBits::Mesh, "meshMain", "data/shaders/MeshShaderMesh.spv");
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/MeshShaderFrag.spv");
            r->BindPushConstant(self, RHIShaderStageBits::Task, 0, sizeof(float));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            auto const& img_wh = r->GetSwapchainExtent();
            r->CmdBeginGraphics(self, cmd, img_wh);
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Task, 0, static_cast<float>(SDL_GetTicks() / 1e3));
            cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                .SetScissor(0, 0, img_wh.x, img_wh.y)
                .DrawMeshTasks(1, 1, 1)
                .EndGraphics();
        });
    renderer->EndSetup();
    SDL_Event event;
    while (!Examples_ShouldClose(window, renderer, swapchain, event))
        Examples_NewFrame(renderer);
    Examples_DestroyVulkan(window, renderer, app);
}
