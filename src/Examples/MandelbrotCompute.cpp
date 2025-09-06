#include <Rendering/Application.hpp>
#include <RHIVulkan/Application.hpp>
#include <Rendering/PSFullscreen.hpp>

using namespace Foundation::Core;
using namespace Foundation::Rendering;
class DemoApp : public Application {
    using Application::Application;
    float m_startTime = 0;
protected:
    struct PC {
        float time;
        RHIExtent2D resolution;
    };
    virtual void RendererSetup() override {
        m_startTime = glfwGetTime();
        ResourceHandle buffer = createResource(
            m_renderer.get(), "Mandelbrot Image",
            RHITextureDesc{
                .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::StorageImage,
                .extent = m_renderer->GetSwapchainExtent3D(),
                .format = RHIResourceFormat::R8G8B8A8_UNORM
            }
        );
        ResourceHandle sampler = createSampler(m_renderer.get(), "Linear Sampler", {});
        createPass(
            m_renderer.get(), "Mandelbrot",
            RHIDeviceQueueType::Compute,
            [=](PassHandle self, Renderer* r) {
                r->BindShader(self, RHIShaderStageBits::Compute, "csMain", "data/shaders/MandelbrotCompute.spv");
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(PC));
                r->BindTextureUAV(self, buffer, "image", { .format = RHIResourceFormat::R8G8B8A8_UNORM });
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd) {                
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, PC{
                    .time = (float)glfwGetTime() - m_startTime,
                    .resolution = r->GetSwapchainExtent()
                });
                r->CmdDispatch(self, cmd, m_renderer->GetSwapchainExtent3D());
            }
        );
        createPSBackbufferBlitPass(m_renderer.get(), "Backbuffer Blit", sampler, buffer);
    }
    virtual void OnSwapchainResize() override {
        InitializeRenderer();
    }
};

int main(int argc, char** argv) {
    // This will actually be slower when you have async compute enabled
    // since the work here aren't parallelized.    
    DemoApp app({ .windowTitle = "Mandelbrot Async Compute", .asyncCompute = true });
    app.Initialize<VulkanApplication>();
    app.RunForever();
}
