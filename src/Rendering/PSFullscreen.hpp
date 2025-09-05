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
                r->BindShader(self, RHIShaderStageBits::Vertex, ".derived/shaders/PSFullscreen_vertMain.spirv");
                setup(self, r);
            },
            [](PassHandle self, Renderer* r, RHI::RHICommandList* cmd) {
                auto const& img_wh = r->GetSwapchainExtent();
                r->CmdBeginGraphics(self, cmd, r->GetSwapchainExtent(), {}, {});
                r->CmdSetPipeline(self, cmd);
                cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                    .SetScissor(0, 0, img_wh.x, img_wh.y);
                cmd->Draw(3);
                cmd->EndGraphics();
            });        
    }
}
