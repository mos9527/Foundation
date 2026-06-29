// Headless Triangle - renders the Hello World triangle into an explicit offscreen
// RTV (no SDL window, no swapchain, no Vulkan WSI) and validates the result via
// texture readback. Mirrors Examples/Triangle.cpp but uses the headless Renderer path
// (Examples_InitVulkan with desc.present = false).
#include "Examples.hpp"
#include <RenderCore/ImmediateContext.hpp>
using namespace Foundation;
using namespace Core;
using namespace RHI;
using namespace RenderCore;

namespace
{
    constexpr RHIExtent2D kExtent{64, 64};
    constexpr RHIResourceFormat kFormat = RHIResourceFormat::R8G8B8A8Unorm;
}

int main(int argc, char** argv)
{
    RendererDesc rendererDesc{
        .present = false, .threadCount = 0, .framesInFlight = 1, .renderExtent = kExtent};
    auto [renderer, app, device, swapchain] =
        Examples_InitVulkan(nullptr /* no window */, argc, argv, rendererDesc);

    renderer->BeginSetup();
    const ResourceHandle output = renderer->CreateResource(
        "Headless Output", RHITextureDesc{
                               .usage = RHITextureUsageBits::RenderTarget | RHITextureUsageBits::TransferSource,
                               .extent = RHIExtent3D{kExtent.x, kExtent.y, 1},
                               .format = kFormat,
                           });
    renderer->CreatePass(
        "Triangle", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r) {
            r->BindTextureRTV(self, output, RHITextureViewDesc{.format = kFormat,
                                                               .range = RHITextureSubresourceRange::Create()});
            r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain",
                          Foundation::Core::PathsResolve("Data/Shaders/Triangle.spv"));
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain",
                          Foundation::Core::PathsResolve("Data/Shaders/Triangle.spv"));
            r->BindPushConstant(self, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0, sizeof(float));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd) {
            auto const img_wh = r->GetRenderExtent();
            r->CmdBeginGraphics(self, cmd, img_wh,
                                {{RHIColorAttachmentLoad{.loadOp = RHIAttachmentLoadOp::Clear, .clearColor = {0, 0, 0, 0}}}});
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0, 0.0f);
            cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                .SetScissor(0, 0, img_wh.x, img_wh.y)
                .Draw(3)
                .EndGraphics();
        }
    );
    renderer->EndSetup();

    // Render one frame headlessly.
    Examples_NewFrame(renderer);
    renderer->WaitForPreviousFrame();
    device->WaitIdle();

    // Read back the offscreen RTV and verify the triangle was actually drawn.
    // Scoped so the readback (which owns device resources) is destroyed before the
    // device/application teardown below.
    size_t drawnPixels = 0;
    {
        auto* outputTex = renderer->DerefResource(output).Get<RHITexture*>();
        const size_t dataSize = static_cast<size_t>(kExtent.x) * kExtent.y * 4;
        ImmediateReadback readback(device.Get(), dataSize + 16);
        readback.Begin();
        {
            auto* cmd = readback.ctx.Get();
            cmd->BeginTransition();
            cmd->SetImageTransition(outputTex,
                {.srcAccess = RHIResourceAccessBits::RenderTargetWrite,
                 .dstAccess = RHIResourceAccessBits::TransferRead,
                 .srcStage = RHIPipelineStageBits::RenderTargetOutput,
                 .dstStage = RHIPipelineStageBits::Transfer,
                 .srcImgLayout = RHITextureLayout::RenderTarget,
                 .dstImgLayout = RHITextureLayout::TransferSrc,
                 .srcImgRange = RHITextureSubresourceRange::Create()});
            cmd->EndTransition();
        }
        char* pixels = readback.Readback(outputTex, dataSize,
                                         RHITextureSubresourceLayer{.aspect = RHITextureAspectFlagBits::Color},
                                         RHIOffset2D{}, kExtent);
        readback.End();
        readback.WaitIdle();

        CHECK_MSG(pixels, "Headless triangle readback failed (out of staging memory)");
        for (size_t i = 0; i < dataSize; i += 4)
        {
            auto const* rgba = reinterpret_cast<unsigned char const*>(pixels + i);
            if (rgba[0] || rgba[1] || rgba[2] || rgba[3])
                ++drawnPixels;
        }

        // Dump the offscreen framebuffer to render.png and open it with the OS default viewer.
        Examples_DumpAndOpenImage("render.png", kExtent, pixels);
    }
    LOG(HeadlessTriangle, LogInfo, "Readback: {}/{} pixels non-clear", drawnPixels, kExtent.x * kExtent.y);
    CHECK_MSG(drawnPixels > 0, "Headless triangle produced no visible pixels; rendering pipeline is broken.");

    Examples_DestroyVulkan(nullptr, renderer, app, device, swapchain);
    fmt::println("HeadlessTriangle: OK");
    return 0;
}
