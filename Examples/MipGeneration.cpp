// Generates a full mip chain for a texture on the GPU and displays the sampled result.
// Uses the shared cameraman image asset as a simple compute mip-generation test.
#include <RenderUtils/CSDebugText.hpp>
#include <RenderUtils/CSMipGeneration.hpp>
#include "Examples.hpp"
#include <stb_image.h>
using namespace RenderUtils;
int main(int argc, char** argv)
{
    SDL_Window* window = SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("MipGeneration Example"), 512, 512,
                                          Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, swapchain, presenter] = Examples_InitVulkan(window, argc, argv,
                                                                  {
                                                                      .asyncCompute = false, /* Nothing to overlap */
                                                                      .threadCount = 0, /* ST recording */
                                                                  });
    {
        int x, y, n;
        stbi_uc* data = stbi_load(Foundation::Core::PathsResolve("Data/Assets/cameraman.jpg").c_str(), &x, &y, &n, 4u);
        CHECK_MSG(data, "Image did not load.");
        auto numMips = static_cast<uint32_t>(std::ceil(std::log2(std::max(x, y)))) + 1;
        auto texture =
            device->CreateTexture({.usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::StorageImage |
                                       RHITextureUsageBits::TransferDestination,
                                   .extent = {static_cast<uint32_t>(x), static_cast<uint32_t>(y), 1},
                                   .format = RHIResourceFormat::R8G8B8A8Unorm,
                                   .mipLevels = numMips});
        // Upload base level
        {
            ImmediateContext im(RHIDeviceQueueType::Graphics, device.Get());
            auto staging = device->CreateBuffer(RHIBufferDesc::CreateStagingDesc(x * y * 4));
            memcpy(staging->Map(), data, x * y * 4);
            im->Begin();
            im->BeginTransition();
            im->SetImageTransition(texture.Get(),
                                   {
                                       .dstAccess = RHIResourceAccessBits::TransferWrite,
                                       .dstStage = RHIPipelineStageBits::Transfer,
                                       .dstImgLayout = RHITextureLayout::TransferDst,
                                       .srcImgRange = RHITextureSubresourceRange::Create(),
                                   });
            im->EndTransition();
            im->CopyBufferToImage(staging.Get(), texture.Get(), RHITextureLayout::TransferDst,
                                  {{RHICommandList::CopyImageRegion{
                                      .srcBufferOffset = 0,
                                      .dstLayer =
                                          RHITextureSubresourceLayer{
                                              .aspect = RHITextureAspectFlagBits::Color,
                                              .mipLevel = 0,
                                              .baseArrayLayer = 0,
                                              .layerCount = 1,
                                          },
                                      .extent =
                                          {
                                              static_cast<uint32_t>(x),
                                              static_cast<uint32_t>(y),
                                              1,
                                          },
                                  }}});
            im->End();
            im.Submit();
            im.WaitIdle();
        }
        stbi_image_free(data);
        renderer->BeginSetup();
        ResourceHandle hdl = renderer->CreateResource("Mip Image", texture.Get());
        ResourceHandle sampler = renderer->CreateSampler({});
        createCSMipGenerationSinglePass(renderer, "Mip Generation", RHIDeviceQueueType::Compute, hdl, hdl,
                                        RHIResourceFormat::R8G8B8A8Unorm, RHIResourceFormat::R8G8B8A8Unorm,
                                        RHITextureAspectFlagBits::Color, RHITextureAspectFlagBits::Color, sampler,
                                        numMips);
        ExampleInputState input{};
        float previewLod = 0.0f;
        const float maxPreviewLod = static_cast<float>(numMips - 1u);
        renderer->CreatePass(
            "Draw Blurred", RHIDeviceQueueType::Graphics, 0u,
            [&](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", Foundation::Core::PathsResolve("Data/Shaders/VSFullscreen.spv"));
                r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", Foundation::Core::PathsResolve("Data/Shaders/MipGenerationBlur.spv"));
                r->BindTextureSRV(self, hdl, "srcTexture", RHIPipelineStageBits::FragmentShader,
                                  {.format = RHIResourceFormat::R8G8B8A8Unorm,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color, 0,
                                                                               texture->mDesc.mipLevels)});
                r->BindTextureSampler(self, sampler, "srcSampler");
                r->BindBackbufferRTV(self);
                r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(float));
            },
            [&](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                auto const& img_wh = r->GetSwapchainExtent();
                r->CmdBeginGraphics(self, cmd, img_wh, {{{RHIAttachmentLoadOp::DontCare}}},
                                    {RHIAttachmentLoadOp::DontCare});
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0,
                                      previewLod);
                cmd->SetViewport(0, 0, img_wh.x, img_wh.y).SetScissor(0, 0, img_wh.x, img_wh.y);
                cmd->Draw(3);
                cmd->EndGraphics();
            });
        createCSDebugTextPassBackBuffer(renderer, "Debug Text", Examples_HudLines(input));
        renderer->EndSetup();
        ExampleFpsCounter fps;
        while (true)
        {
            Examples_BeginFrameInput(input);
            if (Examples_PollEvents(window, renderer, swapchain, input))
                break;

            Examples_BeginControls(input);
            Examples_Text(input, fmt::format("Mip Generation FPS: {}", fps.Update()));
            Examples_Slider(input, "Preview LOD", previewLod, 0.0f, maxPreviewLod, 1.0f);
            previewLod = std::round(previewLod);
            Examples_NewFrame(window, renderer, presenter, swapchain);
        }
        texture.Release(); // Release - destructs with the device
    }
    Examples_DestroyVulkan(window, renderer, app, device, swapchain);
}
