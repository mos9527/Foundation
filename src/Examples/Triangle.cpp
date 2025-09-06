#include <Rendering/Application.hpp>
#include <RHIVulkan/Application.hpp>

using namespace Foundation::Core;
using namespace Foundation::Rendering;

class DemoApp : public Application {
    using Application::Application;
    float m_startTime = 0;
protected:
    virtual void RendererSetup() override {
        m_startTime = glfwGetTime();
        createPass(
            m_renderer.get(), "Triangle",
            RHIDevicePipelineType::Graphics,
            [=](PassHandle self, Renderer* r) {
                r->BindBackbufferRTV(self);
                r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", "data/shaders/Triangle.spv");
                r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/Triangle.spv");
                r->BindPushConstant(self, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0, sizeof(float));
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd) {
                auto const& img_wh = r->GetSwapchainExtent();
                r->CmdBeginGraphics(self, cmd, img_wh);
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0, (float)glfwGetTime() - m_startTime);
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
    DemoApp app({ .windowTitle = "Triangle" });
    app.Initialize<VulkanApplication>();
    app.RunForever();
}
