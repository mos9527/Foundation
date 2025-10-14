#include "Examples.hpp"
namespace Examples
{
    /**
     * @example MandelbrotCompute.cpp
     * Mandelbrot set rendering on the compute shader and automatically synchronizes the result back to display.
     * @example Shaders/MandelbrotCompute.slang
     * Shader courtesy of Inigo Quilez: https://iquilezles.org/articles/mset_smooth/
     */
    class MandelbrotComputeDemoApp : public RenderApplication
    {
        struct PushConstant
        {
            float time;
            float pad;
            RHIExtent2D resolution;
        };
        void OnRendererSetup() override
        {
            ResourceHandle buffer = createResource(
                mRenderer.get(), "Mandelbrot Image",
                RHITextureDesc{.usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::StorageImage,
                               .extent = mRenderer->GetSwapchainExtent3D(),
                               .format = RHIResourceFormat::R8G8B8A8Unorm});
            ResourceHandle sampler = createSampler(mRenderer.get(), {});
            createPass(
                mRenderer.get(), "Mandelbrot", RHIDeviceQueueType::Compute,
                [=](PassHandle self, Renderer* r)
                {
                    r->BindShader(self, RHIShaderStageBits::Compute, "csMain", "data/shaders/MandelbrotCompute.spv");
                    r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(PushConstant));
                    r->BindTextureUAV(
                        self, buffer, "image", RHIPipelineStageBits::ComputeShader,
                        {.format = RHIResourceFormat::R8G8B8A8Unorm, .range = RHITextureSubresourceRange::Create()});
                },
                [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    auto image_wh = r->DerefResource(buffer).Get<RHITexture*>()->mDesc.extent;
                    r->CmdSetPipeline(self, cmd);
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0,
                                          PushConstant{.time = GetApplicationTime(), .resolution = image_wh});
                    r->CmdDispatch(self, cmd, image_wh);
                });
            createPSBackbufferBlitPass(mRenderer.get(), "Backbuffer Blit", sampler, buffer);
        }
    };
} // namespace Examples
int main(int argc, char** argv)
{
    // This will actually be slower when you have async compute enabled
    // Overhead from synchronization is more than the gain from parallelism
    // (which isn't much for this simple example)
    Examples::MandelbrotComputeDemoApp app;
    const bool useAsync = CreateMessageBox("Async Compute", "Enable Async Compute?", MessageBoxType::YesNo,
                                           MessageBoxIcon::Question, MessageBoxResult::Yes) == MessageBoxResult::Yes;
    app.Initialize<VulkanApplication>(
        {.windowTitle = "Mandelbrot Async Compute", .renderer = {.enableAsyncCompute = useAsync}});
    app.RunForever();
}
