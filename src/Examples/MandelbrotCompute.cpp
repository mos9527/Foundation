#include "Examples.hpp"
class DemoApp : public Application {
    struct PushConstants {
        float time;
        RHIExtent2D resolution;
    };
    void RendererSetup() override {
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
            m_renderer.get(), "Mandelbrot", RHIDeviceQueueType::Compute,
            [=](PassHandle self, Renderer* r) {
                r->BindShader(self, RHIShaderStageBits::Compute, "csMain", "data/shaders/MandelbrotCompute.spv");
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(PushConstants));
                r->BindTextureUAV(self, buffer, "image", { .format = RHIResourceFormat::R8G8B8A8_UNORM });
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd) {                
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, PushConstants{
                    .time = GetApplicationTime(),
                    .resolution = r->GetSwapchainExtent()
                });
                r->CmdDispatch(self, cmd, m_renderer->GetSwapchainExtent3D());
            }
        );
        createPSBackbufferBlitPass(m_renderer.get(), "Backbuffer Blit", sampler, buffer);
    }
};

int main(int argc, char** argv) {
    // This will actually be slower when you have async compute enabled
    // Overhead from synchronization is more than the gain from parallelism
    // (which isn't much for this simple example)   
    DemoApp app;
    app.Initialize<VulkanApplication>({ .windowTitle = "Mandelbrot Async Compute", .asyncCompute = true });
    app.RunForever();
}
