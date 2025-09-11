#pragma once
#include "Renderer.hpp"
namespace Foundation::Rendering {
    /**
     * @brief Creates a full-screen triangle pass that writes to the current backbuffer.
     *
     * @param setup is called once during setup phase of the pass, after a fullscreen vertex stage is bound.
     * @param record is called once per frame during execution phase of the pass, after the pipeline is set.
     */
    template<typename FSetup, typename FRecord>
    inline auto* createPSFullscreenPass(
        Renderer* r,
        StringView name,        
        FSetup&& setup,
        FRecord&& record
    ) {
        return createPass(r, name, RHIDeviceQueueType::Graphics,
            [=](PassHandle self, Renderer* r) {
                // NOTE: Swapchain backbuffer for the entierity of a pass is always
                // in ColorAttachmentOptimal layout - until the end of the frame
                // when it's presented.                
                r->BindBackbufferRTV(self);
                r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", "data/shaders/VSFullscreen.spv");
                setup(self, r);
            },
            [=](PassHandle self, Renderer* r, RHI::RHICommandList* cmd) {
                auto const& img_wh = r->GetSwapchainExtent();
                r->CmdBeginGraphics(self, cmd, img_wh, {}, {});
                r->CmdSetPipeline(self, cmd);
                record(self, r, cmd);
                cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                    .SetScissor(0, 0, img_wh.x, img_wh.y);
                cmd->Draw(3);
                cmd->EndGraphics();
            });        
    }
    /**
     * @brief Creates a full-screen triangle pass that renders a texture
     * to the current backbuffer.
     */
    inline auto* createPSBackbufferBlitPass(
        Renderer* r,
        StringView name,
        ResourceHandle copy_sampler,
        ResourceHandle copy_source
    ) {
        return createPSFullscreenPass(
            r,
            name,
            [=](PassHandle self, Renderer* r) {
                r->BindTextureSampler(self, copy_sampler, "sampler");
                r->BindTextureSRV(self, copy_source, "srcTexture", RHIPipelineStageBits::FragmentShader, {
                    .format = RHIResourceFormat::R8G8B8A8_UNORM,
                    .range = RHITextureSubresourceRange::Create()
                });
                r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/PSCopy.spv");
            },
            [](PassHandle, Renderer*, RHI::RHICommandList* cmd) {}
        );
    }
}
