#include "Examples.hpp"
namespace Examples {
    /**
     * @example Triangle.cpp
     * Animated triangle example.
     * @example Shaders/Triangle.slang
     */
    class TriangleDemoApp : public RenderApplication {
        void RendererSetup() override {
            createPass(
                m_renderer.get(), "Triangle", RHIDeviceQueueType::Graphics,
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
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0, GetApplicationTime());
                    cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                        .SetScissor(0, 0, img_wh.x, img_wh.y)
                        .Draw(3)
                        .EndGraphics();
                }
            );
        }
    };

}
int main(int argc, char** argv) {
    Examples::TriangleDemoApp app;
    app.Initialize<VulkanApplication>({ .windowTitle = "Triangle" });
    app.RunForever();
}
