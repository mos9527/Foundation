#include <Rendering/Application.hpp>
#include <RHIVulkan/Application.hpp>
#include <Rendering/PSFullscreen.hpp>

using namespace Foundation::Core;
using namespace Foundation::Rendering;

class DemoApp : public Application {
    using Application::Application;
    float m_startTime = 0;
protected:
    virtual void RendererSetup() override {
        m_startTime = glfwGetTime();
        createPSFullscreenPass(
            m_renderer.get(), "SDF2D",
            [=](PassHandle self, Renderer* r) {
                r->BindShader(self, RHIShaderStageBits::Fragment, "data/shaders/SDF2D_fragMain.spirv");
                r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(float));
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd) { r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, (float)glfwGetTime() - m_startTime); }
        );
    }
    virtual void OnSwapchainResize() override {        
        InitializeRenderer();
    }
};

int main(int argc, char** argv) {
    DemoApp app;
    app.Initialize<VulkanApplication>({.windowTitle = "SDF2D Renderer"});
    app.RunForever();
}
