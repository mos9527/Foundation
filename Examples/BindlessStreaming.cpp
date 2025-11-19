#include <RenderCore/Bindless.hpp>
#include <RenderCore/ImmediateContext.hpp>
#include <RenderCore/Streaming.hpp>
#include <RenderUtils/CSDebugText.hpp>
#include <RenderUtils/PSFullscreen.hpp>
using namespace RenderUtils;
struct PushConstant
{
    float time;
    uint32_t binding;
    uint32_t mipReady; // Highest mip level that is ready
};
Vector<uint32_t> generatedData(4096 * 4096, GLOBAL_ALLOC); // Max size for 4096x4096 RGBA8
Span<uint32_t> generateCheckerboardMip(uint32_t mipLevel)
{
    uint32_t dim = 1u << (12 - mipLevel);
    Span<uint32_t> mipData = Span<uint32_t>(generatedData.data(), dim * dim);
    for (uint32_t y = 0; y < dim; y++)
    {
        for (uint32_t x = 0; x < dim; x++)
        {
            bool isWhite = (x+y+1) & 1;
            mipData[y * dim + x] = isWhite ? 0xFFFFFFFF : 0xFF000000;
        }
    }
    return mipData;
}
int main()
{
    SDL_Window* window = SDL_CreateWindow("BindlessStreaming Example", 1024, 1024, Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, swapchain] =
        Examples_InitVulkan(window,
                            {
                                .asyncCompute = false, .threadCount = 0, /* ST recording */
                            });
    CSDebugTextData lines[5]{};
    lines[0].col32 = lines[1].col32 = lines[2].SetColor(255,255,0,255);
    lines[0].x = lines[0].y = 16, lines[0].SetText("Bindless Streaming - Enter to submit a new mip");
    {
        BindlessPool bindings(device.Get(), GLOBAL_ALLOC, {.maxBindings = 1});
        StreamingPool stream(device.Get(), GLOBAL_ALLOC, {.maxTransferPerSubmit = 16});
        // Lots of mips to show LOD streaming
        auto tex = device->CreateTexture({
            .resource = {.heap = RHIDeviceHeapType::Local, .shared = true},
            // ^^^ Necessary to share. StreamingPool only works in Transfer queue
            .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
            .extent = {4096, 4096, 1},
            .format = RHIResourceFormat::B8G8R8A8Unrom,
            .mipLevels = 13, // 1 + log2(4096)
        });
        auto view =
            tex->CreateTextureView(RHITextureViewDesc{
                .format = RHIResourceFormat::B8G8R8A8Unrom,
                .range = RHITextureSubresourceRange::Create(
                    RHITextureAspectFlagBits::Color, 0, tex->mDesc.mipLevels, 0, 1
                )
            });
        uint32_t binding = bindings.Allocate(view.Release().Get()); // OK to release - textures own the view.
        // Oneshot command to transition the whole texture to TransferDst
        // Alternatively - create the textures in linear tiling so we can just map and write directly
        {
            ImmediateContext im(RHIDeviceQueueType::Graphics, device.Get());
            im->Begin();
            im->BeginTransition();
            im->SetImageTransition(tex.Get(), {
                .dstAccess = RHIResourceAccessBits::TransferWrite,
                .dstStage = RHIPipelineStageBits::Transfer,
                .dstImgLayout = RHITextureLayout::TransferDst,
                .srcImgRange = RHITextureSubresourceRange::Create(
                    RHITextureAspectFlagBits::Color, 0, tex->mDesc.mipLevels, 0, 1
                ),
            });
            im->EndTransition();
            im->End();
            im.Submit();
            device->WaitIdle();
        }
        renderer->BeginSetup();
        ResourceHandle nearSampler = renderer->CreateSampler({
            .filter = {
                .minFilter = RHIDeviceSampler::SamplerDesc::Filter::NearestNeighbor,
                .magFilter = RHIDeviceSampler::SamplerDesc::Filter::NearestNeighbor,
            }
        });
        uint32_t mipReady = 13, mipView = 13;
        auto transitionPass = renderer->CreatePass("Transition", RHIDeviceQueueType::Graphics, 0,
            FSetupDefault{}, [&](PassHandle, Renderer*, RHICommandList* cmd)
            {
                if (mipReady != mipView)
                {
                    // Transition - mipView has just been uploaded
                    cmd->BeginTransition();
                    cmd->SetImageTransition(tex.Get(), {
                        .srcAccess = RHIResourceAccessBits::TransferWrite,
                        .dstAccess = RHIResourceAccessBits::ShaderRead,
                        .srcStage = RHIPipelineStageBits::Transfer,
                        .dstStage = RHIPipelineStageBits::FragmentShader,
                        .srcImgLayout = RHITextureLayout::TransferDst,
                        .dstImgLayout = RHITextureLayout::ShaderReadOnly,
                        .srcImgRange = RHITextureSubresourceRange::Create(
                            RHITextureAspectFlagBits::Color, mipView, 1, 0, 1
                        ),
                    });
                    cmd->EndTransition();
                    mipReady = mipView;
                }
            });
        createPSFullscreenPass(
            renderer, "Atlas Display",
            [&](PassHandle self, Renderer* r)
            {
                // Ensure transition pass happens first, in both recording
                // and GPU execution.
                r->BindPass(self, transitionPass);
                r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/BindlessStreaming.spv");
                r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(PushConstant));
                r->BindTextureSampler(self, nearSampler, "sampler");
                r->BindDescriptorSet(self, "textures",  bindings.GetDescriptorSetLayout());
            },
            [&](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                PushConstant pc{
                    .time = 0.0f,
                    .binding = binding,
                    .mipReady = mipReady
                };
                r->CmdBindDescriptorSet(self, cmd, "textures", bindings.GetDescriptorSet());
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, pc);
            });
        createCSDebugTextPassBackBuffer(renderer, "Debug Text", lines);
        renderer->EndSetup();
        ExampleFpsCounter fps;
        SDL_Event event;

        StreamingFuture lastFuture;
        ThreadPool uploadPool(1, 1, GLOBAL_ALLOC, "UploadPool");
        while (!Examples_ShouldClose(window, renderer, swapchain, &event))
        {
            lines[1].x = 16, lines[1].y = 40, lines[1].SetText(fmt::format("FPS: {}", fps.Update()));
            lines[2].x = 16, lines[2].y = 64,
            lines[2].SetText(fmt::format("Streaming Pool: {}", stream.DbgGetStatistics()));
            if (lastFuture.valid() && lastFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            {
                LOG(Example, LogDebug, "Mip {} upload complete", mipView);
                mipView--, lastFuture = {};
            }
            if (mipView != 0 && !uploadPool.GetPendingJobCount() && !lastFuture.valid() && event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_RETURN)
            {
                // Upload a new mip, bottom up
                uint32_t mipPrep = mipView - 1;
                const uint32_t dim = 1u << (12 - mipPrep);
                // Stream ops are thread-safe by design. Use a thread pool to simulate real-world usage
                uploadPool.Push([=, &lastFuture, &stream, &tex]
                {
                    LOG(Upload, LogDebug, "Worker streaming mip {} {}x{}", mipPrep, dim, dim);
                    lastFuture = stream.Write(generateCheckerboardMip(mipPrep).AsBytes(), tex.Get(), RHITextureAspectFlagBits::Color, mipPrep, 0);
                });
            }
            Examples_NewFrame(renderer);
        }
    }
    Examples_DestroyVulkan(window, renderer, app, device, swapchain);
}
