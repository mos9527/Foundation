#include "Examples.hpp"
class DemoApp : public Application {
    void RendererSetup() override {
        createPSFullscreenPass(
            m_renderer.get(), "SDF2D",
            [=](PassHandle self, Renderer* r) {
                r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/SDF2D.spv");
                r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(float));
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd) {
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, GetApplicationTime());
            }
        );
    }
};

int main(int argc, char** argv) {
    DemoApp app;
    app.Initialize<VulkanApplication>({ .windowTitle = "SDF2D" });
    app.RunForever();
}
