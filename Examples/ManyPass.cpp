#include "Examples.hpp"
namespace Examples {
    /**
     * @example ManyPass.cpp
     * Example using 2 graphics passes with alpha blending on the RenderTarget backbuffer.
     */
    class ManyPassDemoApp : public RenderApplication {
        void OnRendererSetup() override {
            createPSFullscreenPass(
                mRenderer.get(), "ManyPass 1",
                [=](PassHandle self, Renderer* r) {
                    r->BindShader(self, RHIShaderStageBits::Fragment, "pass1", "data/shaders/ManyPass.spv");
                    r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(float));
                    r->BindBackbufferRTV(self);
                },
                [=, this](PassHandle self, Renderer* r, RHICommandList* cmd) {
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, GetApplicationTime());
                }
            );
            createPSFullscreenPass(
                mRenderer.get(), "ManyPass 2",
                [=](PassHandle self, Renderer* r)
                {
                    r->BindShader(self, RHIShaderStageBits::Fragment, "pass2", "data/shaders/ManyPass.spv");
                    r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(float));
                    r->BindBackbufferRTV(self,
                                         RHIPipelineState::PipelineStateDesc::Attachment::Blending::GetAlphaBlending());
                },
                [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
                { r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, GetApplicationTime()); });
        }
    };

}
int main(int argc, char** argv) {
    Examples::ManyPassDemoApp app;
    app.Initialize<VulkanApplication>({ .windowTitle = "ManyPass" });
    app.RunForever();
}
