#include "CopyToSwapchainPass.hpp"
using namespace Foundation;
void CopyToSwapchainPass::Setup(PassHandle self, Renderer& r) {
    r.BindTextureCopySrc(self, source);    
}
void CopyToSwapchainPass::Record(PassHandle self, Renderer& r, RHICommandList* cmd) {
    cmd->BeginTransition()
        .SetImageTransition(
            r.GetCurrentBackbuffer(),
            {
                .dst_access = RHIResourceAccessBits::TransferWrite,
                .dst_img_layout = RHITextureLayout::TransferDst,
            }
            );
    cmd->EndTransition();
    cmd->CopyImage(
        r.DerefResource(source).Get<RHITexture*>(),
        RHITextureLayout::TransferSrc,
        r.GetCurrentBackbuffer(),
        RHITextureLayout::TransferDst,
        {
            {{.extent = { r.GetSwapchainExtent().x , r.GetSwapchainExtent().y, 1 } }}
        }
    );
}
