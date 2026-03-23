#pragma once
#include <Core/Paths.hpp>
#include <RenderCore/Renderer.hpp>
namespace Foundation::RenderUtils {
    using namespace RenderCore;
    /**
     * @brief Creates a full-screen triangle pass that writes to the current backbuffer.
     *
     * @param setup is called once during setup phase of the pass, after a fullscreen vertex stage is bound.
     * @param record is called once per frame during execution phase of the pass, after the pipeline is set.
     */
    template<typename FSetup, typename FRecord>
    PassHandle createPSFullscreenPass(
        Renderer* r,
        StringView name,        
        FSetup&& setup,
        FRecord&& record
    ) {
        return r->CreatePass(name, RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r) {
                r->BindBackbufferRTV(self);
                r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", Foundation::Core::PathsResolve("data/shaders/VSFullscreen.spv"));
                setup(self, r);
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                auto const& img_wh = r->GetSwapchainExtent();
                r->CmdSetPipeline(self, cmd);
                record(self, r, cmd);
                r->CmdBeginGraphics(self, cmd, img_wh, {{RHIColorAttachmentLoad{RHIAttachmentLoadOp::DontCare}}},
                                    {RHIAttachmentLoadOp::DontCare}); // No clear necessary
                cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                    .SetScissor(0, 0, img_wh.x, img_wh.y);
                cmd->Draw(3);
                cmd->EndGraphics();
            });        
    }
    template<typename FSetup>
    PassHandle createPSFullscreenPass(
        Renderer* r,
        StringView name,        
        FSetup&& setup)
    {
        return createPSFullscreenPass(r, name, std::forward<FSetup>(setup), FRecordDefault{});
    }
    /**
     * @brief Creates a full-screen triangle pass that writes to an arbitrary texture RTV
     *        instead of the backbuffer.
     *
     * @param rtvTexture  Resource handle of the texture to render into.
     * @param rtvViewDesc View descriptor for the render target (format, subresource range, etc.).
     * @param setup       Called once during setup phase, after fullscreen vertex stage is bound.
     * @param record      Called once per frame during execution phase, after the pipeline is set.
     */
    template<typename FSetup, typename FRecord>
    PassHandle createPSFullscreenPassRTV(
        Renderer* r,
        StringView name,
        ResourceHandle rtvTexture,
        RHITextureViewDesc const& rtvViewDesc,
        FSetup&& setup,
        FRecord&& record
    ) {
        return r->CreatePass(name, RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r) {
                r->BindTextureRTV(self, rtvTexture, rtvViewDesc);
                r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", Foundation::Core::PathsResolve("data/shaders/VSFullscreen.spv"));
                setup(self, r);
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                auto const& img_wh = r->GetSwapchainExtent();
                r->CmdSetPipeline(self, cmd);
                record(self, r, cmd);
                r->CmdBeginGraphics(self, cmd, img_wh, {{RHIColorAttachmentLoad{RHIAttachmentLoadOp::DontCare}}},
                                    {RHIAttachmentLoadOp::DontCare});
                cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                    .SetScissor(0, 0, img_wh.x, img_wh.y);
                cmd->Draw(3);
                cmd->EndGraphics();
            });
    }
    template<typename FSetup>
    PassHandle createPSFullscreenPassRTV(
        Renderer* r,
        StringView name,
        ResourceHandle rtvTexture,
        RHITextureViewDesc const& rtvViewDesc,
        FSetup&& setup)
    {
        return createPSFullscreenPassRTV(r, name, rtvTexture, rtvViewDesc,
                                         std::forward<FSetup>(setup), FRecordDefault{});
    }
    /**
     * @brief Creates a full-screen triangle pass that renders a texture
     * to the current backbuffer.
     */
    inline PassHandle createPSBackbufferBlitPass(
        Renderer* r,
        StringView name,
        ResourceHandle copy_sampler,
        ResourceHandle copy_source,
        RHIResourceFormat srcFormat = RHIResourceFormat::R8G8B8A8Unorm
    ) {
        return createPSFullscreenPass(
            r,
            name,
            [=](PassHandle self, Renderer*) {
                r->BindTextureSampler(self, copy_sampler, "sampler");
                r->BindTextureSRV(self, copy_source, "srcTexture", RHIPipelineStageBits::FragmentShader, {
                    .format = srcFormat,
                    .range = RHITextureSubresourceRange::Create()
                });
                r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", Foundation::Core::PathsResolve("data/shaders/PSCopy.spv"));
            },
            [](PassHandle, Renderer*, RHICommandList*) {}
        );
    }
}
