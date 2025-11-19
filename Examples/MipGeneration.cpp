#define STB_IMAGE_IMPLEMENTATION
#include <RenderUtils/CSDebugText.hpp>
#include <RenderUtils/CSMipGeneration.hpp>
#include <stb_image.h>
using namespace RenderUtils;
int main()
{
    SDL_Window* window = SDL_CreateWindow("MipGeneration Example", 512, 512, Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, swapchain] = Examples_InitVulkan(window,
                                                                  {
                                                                      .threadCount = 0 /* ST recording */
                                                                  });
    CSDebugTextData lines[5]{};
    lines[0].x = lines[0].y = 16, lines[0].SetText("Mip Generation");
    {
        int x, y, n;
        stbi_uc* data = stbi_load("data/assets/cameraman.jpg", &x, &y, &n, 4u);
        CHECK_MSG(data, "Image did not load.");
        auto texture =
            device->CreateTexture({.usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::StorageImage |
                                       RHITextureUsageBits::TransferDestination,
                                   .extent = {static_cast<uint32_t>(x), static_cast<uint32_t>(y), 1},
                                   .format = RHIResourceFormat::R8G8B8A8Unorm,
                                   .mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::min(x, y)))) + 1u});
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
        // Release - texture is part of the renderer now
        ResourceHandle hdl = renderer->CreateResource("Mip Image", texture.Get());
        ResourceHandle sampler = renderer->CreateSampler({});
        createCSMipGenerationPasses(renderer, "Mip Generation", RHIDeviceQueueType::Compute, hdl, hdl, {x, y},
                                    RHITextureAspectFlagBits::Color, RHIResourceFormat::B8G8R8A8Unrom,
                                    RHITextureAspectFlagBits::Color, RHIResourceFormat::B8G8R8A8Unrom);
        float blurAmount = 0.0f; // [0,1]
        renderer->CreatePass("Draw Blurred", RHIDeviceQueueType::Graphics, 0u,
        [&](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", "data/shaders/VSFullscreen.spv");
                r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/MipGenerationBlur.spv");
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
                r->CmdBeginGraphics(self, cmd, img_wh, {}, {});
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, blurAmount * texture->mDesc.mipLevels);
                cmd->SetViewport(0, 0, img_wh.x, img_wh.y).SetScissor(0, 0, img_wh.x, img_wh.y);
                cmd->Draw(3);
                cmd->EndGraphics();
            }
        );
        createCSDebugTextPassBackBuffer(renderer, "Debug Text", lines);
        renderer->EndSetup();
        SDL_Event event;
        ExampleFpsCounter fps;
        while (!Examples_ShouldClose(window, renderer, swapchain, &event))
        {
            lines[1].x = 16, lines[1].y = 40, lines[1].SetText(fmt::format("FPS: {}", fps.Update()));
            if (event.type == SDL_EVENT_MOUSE_WHEEL)
            {
                blurAmount = std::clamp(blurAmount + event.wheel.y * 0.05f, 0.0f, 1.0f);
            }
            lines[2].x = 16; lines[2].y = 64; lines[2].SetText(fmt::format("Blur (MWHEEL): {:.2f}", blurAmount));
            Examples_NewFrame(renderer);
        }
        texture.Release(); // Release - destructs with the device
    }
    Examples_DestroyVulkan(window, renderer, app, device, swapchain);
}
