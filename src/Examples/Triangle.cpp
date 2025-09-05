#include <Rendering/Application.hpp>
#include <RHIVulkan/Application.hpp>

using namespace Foundation::Core;
using namespace Foundation::Rendering;

class ModelViewer : public Application {
    using Application::Application;
protected:
    virtual void RendererSetup() override {
        createPass(
            m_renderer.get(), "Triangle",
            RHIDevicePipelineType::Graphics,
            [=](PassHandle self, Renderer* r) {
                r->BindBackbufferRTV(self);
                r->BindShader(self, RHIShaderStageBits::Vertex, "data/shaders/Triangle_vertMain.spirv");
                r->BindShader(self, RHIShaderStageBits::Fragment, "data/shaders/Triangle_fragMain.spirv");
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd) {
                auto const& img_wh = r->GetSwapchainExtent();
                r->CmdBeginGraphics(self, cmd, img_wh);
                r->CmdSetPipeline(self, cmd);
                cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                    .SetScissor(0, 0, img_wh.x, img_wh.y)
                    .Draw(3)
                    .EndGraphics();
            }
        );
    }
    virtual void OnSwapchainResize() override {        
        InitializeRenderer();
    }
};

int main(int argc, char** argv) {
    ModelViewer app;
    app.Initialize<VulkanApplication>();
    app.RunForever();
}
