#include "Examples.hpp"
namespace Examples {
    /**
     * @example SDF2D.cpp
     * 2D Signed Distance Field (SDF) example.
     * @example Shaders/SDF2D.slang  
     * Shader courtesy of Inigo Quilez: https://iquilezles.org/articles/distfunctions2d/
     */
    class SDFDemoApp : public RenderApplication {
        void RendererSetup() override {
            createPSFullscreenPass(
                m_renderer.get(), "SDF2D",
                [=](PassHandle self, Renderer* r) {
                    r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/SDF2D.spv");
                    r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(float));
                },
                [=, this](PassHandle self, Renderer* r, RHICommandList* cmd) {
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, GetApplicationTime());
                }
            );
        }
    };

}
int main(int argc, char** argv) {
    Examples::SDFDemoApp app;
    app.Initialize<VulkanApplication>({ .windowTitle = "SDF2D" });
    app.RunForever();
}
