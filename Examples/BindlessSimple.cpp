#include <RenderCore/Bindless.hpp>
#include <RenderUtils/CSDebugText.hpp>
#include <RenderUtils/PSFullscreen.hpp>
using namespace RenderUtils;
struct PushConstant
{
    float time;
    uint32_t total;
    uint32_t first;
};
constexpr uint32_t kNumTextures = 64;
int main()
{
    SDL_Window* window = SDL_CreateWindow("BindlessSimple Example", 800, 600, Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, swapchain] =
        Examples_InitVulkan(window,
                            {
                                .enableAsyncCompute = false, .threads = 0, /* ST recording */
                            });
    CSDebugTextData lines[5];
    lines[0].x = lines[0].y = 16, lines[0].SetText("Bindless Simple");
    {
        BindlessPool bindings(device.Get(), GLOBAL_ALLOC, {.maxBindings = kNumTextures});
        Vector<RHIDeviceScopedObjectHandle<RHITexture>> textures(kNumTextures, GLOBAL_ALLOC);
        // Prepare textures
        {
            ImmediateContext im(RHIDeviceQueueType::Graphics, device.Get());
            auto staging = device->CreateBuffer(RHIBufferDesc::CreateStagingDesc(256 * 256 * 4 * kNumTextures));
            const auto format = RHIResourceFormat::R8G8B8A8Unorm;
            const auto extent = RHIExtent3D{256, 256, 1};
            uint32_t offset = 0;
            im->Begin();
            for (auto& tex : textures)
            {
                auto pixels = Span<uint32_t>(static_cast<uint32_t*>(staging->Map()) + offset, extent.x * extent.y);
                tex = device->CreateTexture({
                    .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
                    .extent = extent,
                    .format = format,
                });
                auto view = tex->CreateTextureView(
                    RHITextureViewDesc{.format = format, .range = RHITextureSubresourceRange::Create()});
                // Fill with a color pattern
                uint32_t color = 0xFF000000 | ((rand() % 256) << 16) | ((rand() % 256) << 8) | (rand() % 256);
                for (size_t i = 0; i < pixels.size(); ++i)
                    pixels[i] = color;
                bindings.Allocate(view.Release().Get()); // OK to release - textures own the view.
                // Upload image
                im->BeginTransition();
                im->SetImageTransition(tex.Get(), {
                    .dstAccess = RHIResourceAccessBits::TransferWrite,
                    .dstStage = RHIPipelineStageBits::Transfer,
                    .dstImgLayout = RHITextureLayout::TransferDst,
                    .srcImgRange = RHITextureSubresourceRange::Create(),
                });
                im->EndTransition();
                im->CopyBufferToImage(staging.Get(), tex.Get(), RHITextureLayout::TransferDst,
                                      {{RHICommandList::CopyImageRegion{
                                          .srcBufferOffset = offset * 4,
                                          .dstLayer = RHITextureSubresourceLayer{
                                              .aspect = RHITextureAspectFlagBits::Color,
                                              .mipLevel = 0,
                                              .baseArrayLayer = 0,
                                              .layerCount = 1,
                                          },
                                          .extent = extent
                                      }}});
                im->BeginTransition();
                im->SetImageTransition(tex.Get(), {
                    .srcAccess = RHIResourceAccessBits::TransferRead,
                    .dstAccess = RHIResourceAccessBits::ShaderRead,
                    .srcStage = RHIPipelineStageBits::Transfer,
                    .dstStage = RHIPipelineStageBits::FragmentShader,
                    .srcImgLayout = RHITextureLayout::TransferDst,
                    .dstImgLayout = RHITextureLayout::ShaderReadOnly,
                    .srcImgRange = RHITextureSubresourceRange::Create(),
                });
                im->EndTransition();
                offset += extent.x * extent.y;
            }
            im->End();
            im.Submit();
            im.WaitIdle();
        }
        renderer->BeginSetup();
        ResourceHandle linSampler = renderer->CreateSampler({});
        createPSFullscreenPass(
            renderer, "Atlas Display",
            [&](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/BindlessSimple.spv");
                r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(PushConstant));
                r->BindTextureSampler(self, linSampler, "sampler");
                r->BindDescriptorSet(self, "textures", bindings.GetDescriptorSet(), bindings.GetDescriptorSetLayout());
            },
            [&](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0,
                                      PushConstant{.time = Examples_GetTime(), .total = kNumTextures, .first = 0});
            });
        createCSDebugTextPassBackBuffer(renderer, "Debug Text", lines);
        renderer->EndSetup();

        ExampleFpsCounter fps;
        while (!Examples_ShouldClose(window, renderer, swapchain))
        {
            lines[1].x = 16, lines[1].y = 40, lines[1].SetText(fmt::format("FPS: {}", fps.Update()));
            Examples_NewFrame(renderer);
        }
    }
    Examples_DestroyVulkan(window, renderer, app, device, swapchain);
}
