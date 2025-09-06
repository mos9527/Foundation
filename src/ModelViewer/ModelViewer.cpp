#include "ModelViewer.hpp"
/**
* @brief Model Viewer Application
*/
class ModelViewer : public RenderApplication {
    virtual void RendererSetup() override {
        ResourceHandle gb_albedo, cp_sampler;
        gb_albedo = createResource(
            m_renderer.get(),
            "GBuffer Albedo",
            RHITextureDesc{
                .usage = RHITextureUsageBits::RenderTarget | RHITextureUsageBits::SampledImage,
                .extent = m_renderer->GetSwapchainExtent3D(),
                .format = RHIResourceFormat::R8G8B8A8_UNORM
            });
        cp_sampler = createSampler(m_renderer.get(), "Copy Sampler", {});
        createPass(
            m_renderer.get(), "GBuffer", RHIDeviceQueueType::Graphics,
            [=](PassHandle self, Renderer* r) {
                r->BindTextureRTV(self, gb_albedo, { .format = RHIResourceFormat::R8G8B8A8_UNORM });
                r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", "data/shaders/Mesh.spv");
                r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/Mesh.spv");
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd) {
                auto const& img_wh = r->DerefResource(gb_albedo).Get<RHITexture*>()->m_desc.extent;
                r->CmdBeginGraphics(self, cmd, img_wh);
                r->CmdSetPipeline(self, cmd);
                cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                    .SetScissor(0, 0, img_wh.x, img_wh.y);
                cmd->Draw(3); // Another triangle...sigh
                cmd->EndGraphics();
            }
        );
        createPSBackbufferBlitPass(m_renderer.get(), "Backbuffer Blit", cp_sampler, gb_albedo);
    }
};

int main(int argc, char** argv) {
    ModelViewer app;
    app.Initialize<VulkanApplication>({ .windowTitle = "Model Viewer" });
    app.RunForever();
}
