#include "Examples.hpp"
namespace Examples
{
    /**
     * @example MeshShader.cpp
     * Mesh Shader example.
     * @example Shaders/MeshShader.slang
     * Simple shader to render lots of triangles. In mesh shader pipeline.
     */
    class TriangleDemoApp : public RenderApplication
    {
        void OnRendererSetup() override
        {
            createPass(
                mRenderer.get(), "Mesh Task", RHIDeviceQueueType::Graphics,
                [=](PassHandle self, Renderer* r)
                {
                    r->BindBackbufferRTV(self);
                    r->BindShader(self, RHIShaderStageBits::Task, "taskMain", "data/shaders/MeshShaderTask.spv");
                    r->BindShader(self, RHIShaderStageBits::Mesh, "meshMain", "data/shaders/MeshShaderMesh.spv");
                    r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/MeshShaderFrag.spv");
                    r->BindPushConstant(self, RHIShaderStageBits::Task, 0, sizeof(float));
                },
                [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    auto const& img_wh = r->GetSwapchainExtent();
                    r->CmdBeginGraphics(self, cmd, img_wh);
                    r->CmdSetPipeline(self, cmd);
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Task, 0, GetApplicationTime());
                    cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                        .SetScissor(0, 0, img_wh.x, img_wh.y)
                        .DrawMeshTasks(1, 1, 1)
                        .EndGraphics();
                });
        }
    };

} // namespace Examples
int main(int argc, char** argv)
{
    Examples::TriangleDemoApp app;
    app.Initialize<VulkanApplication>({.windowTitle = "Mesh Shader"});
    app.RunForever();
}
