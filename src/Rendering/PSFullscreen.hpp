#pragma once
#include "Renderer.hpp"
namespace Foundation::Rendering {
    /// <summary>
    /// Creates a full-screen triangle pass that writes to the current backbuffer.
    /// </summary>    
    template<typename FSetup>
    inline auto* createPSFullscreenPass(
        Renderer* r,
        std::string const& name,        
        FSetup&& setup
    ) {
        return createPass(r, name, RHIDevicePipelineType::Graphics,
            [=](PassHandle self, Renderer* r) {
                // NOTE: Swapchain backbuffer for the entierity of a pass is always
                // in ColorAttachmentOptimal layout - until the end of the frame
                // when it's presented.                
                r->BindBackbufferRTV(self);
                r->BindShader(self, RHIShaderStageBits::Vertex, "data/shaders/PSFullscreen_vertMain.spirv");
                setup(self, r);
            },
            [](PassHandle self, Renderer* r, RHI::RHICommandList* cmd) {
                auto const& img_wh = r->GetSwapchainExtent();
                r->CmdBeginGraphics(self, cmd, img_wh, {}, {});
                r->CmdSetPipeline(self, cmd);
                cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                    .SetScissor(0, 0, img_wh.x, img_wh.y);
                cmd->Draw(3);
                cmd->EndGraphics();
            });        
    }
    /// <summary>
    /// Creates a full-screen triangle pass that copies from a source texture to the backbuffer.
    /// </summary>
    inline auto* createPSBackbufferBlitPass(
        Renderer* r,
        std::string const& name,
        ResourceHandle copy_sampler,
        ResourceHandle copy_source
    ) {
        return createPSFullscreenPass(
            r,
            name,
            [=](PassHandle self, Renderer* r) {
                r->BindTextureSampler(self, copy_sampler, "sampler");
                r->BindTextureSRV(self, copy_source, "srcTexture", { .format = RHIResourceFormat::R8G8B8A8_UNORM });
                r->BindShader(self, RHIShaderStageBits::Fragment, "data/shaders/PSCopy_fragMain.spirv");
            }
        );
    }
}
