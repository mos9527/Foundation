#include "Examples.hpp"
namespace Examples {
    /**
     * @example MandelbrotCompute.cpp
     * Mandelbrot set rendering on the compute shader and automatically synchronizes the result back to display.
     * @example Shaders/MandelbrotCompute.slang
     * Shader courtesy of Inigo Quilez: https://iquilezles.org/articles/mset_smooth/
     */
    class MandelbrotComputeDemoApp : public RenderApplication {
        struct PushConstants {
            float time;
            float pad;
            RHIExtent2D resolution;
        };
        void OnRendererSetup() override {
            ResourceHandle buffer = createResource(
                m_renderer.get(), "Mandelbrot Image",
                RHITextureDesc{
                    .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::StorageImage,
                    .extent = m_renderer->GetSwapchainExtent3D(),
                    .format = RHIResourceFormat::R8G8B8A8_UNORM
                }
            );
            ResourceHandle sampler = createSampler(m_renderer.get(), {});
            createPass(
                m_renderer.get(), "Mandelbrot", RHIDeviceQueueType::Compute,
                [=](PassHandle self, Renderer* r) {
                    r->BindShader(self, RHIShaderStageBits::Compute, "csMain", "data/shaders/MandelbrotCompute.spv");
                    r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(PushConstants));
                    r->BindTextureUAV(self, buffer, "image", RHIPipelineStageBits::ComputeShader, {
                        .format = RHIResourceFormat::R8G8B8A8_UNORM,
                        .range = RHITextureSubresourceRange::Create()
                    });
                },
                [=, this](PassHandle self, Renderer* r, RHICommandList* cmd) {
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
}
int main(int argc, char** argv) {
    // This will actually be slower when you have async compute enabled
    // Overhead from synchronization is more than the gain from parallelism
    // (which isn't much for this simple example)
    Examples::MandelbrotComputeDemoApp app;
    const bool useAsync = MessageBox("Async Compute", "Enable Async Compute?", MessageBoxType::YesNo, MessageBoxIcon::Question, MessageBoxResult::Yes) == MessageBoxResult::Yes;
    app.Initialize<VulkanApplication>({ .windowTitle = "Mandelbrot Async Compute", .asyncCompute = useAsync });
    app.RunForever();
}
