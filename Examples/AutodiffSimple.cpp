// GPU BC7-style block compression via gradient descent. Port of NVIDIA Falcor's TinyBC sample.
#include <RenderUtils/CSDebugText.hpp>
#include "Examples.hpp"
#include <stb_image.h>
using namespace RenderUtils;
#pragma pack(push, 1)
struct AutodiffParams
{
    int32_t useAdam;
    int32_t numOptimizationSteps;
    float lr;             // learning rate
    float adamBeta1;
    float adamBeta2;
    uint2 textureDim;
};
#pragma pack(pop)
int main(int argc, char** argv)
{
    SDL_Window* window = SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("AutodiffSimple Example"), 1024, 1024,
                                          Examples_SDLWindowFlagsVulkan);
    auto ctx = Examples_InitVulkan(window, argc, argv,
                                                                  {
                                                                      .asyncCompute = false, /* Nothing to overlap */
                                                                      .threadCount = 0, /* ST recording */
                                                                  });
    {
        int x, y, n;
        stbi_uc* data = stbi_load(ctx.app->ResolveRelativePathBase("Data/Assets/cameraman.jpg").c_str(), &x, &y, &n, 4u);
        CHECK_MSG(data, "Image did not load.");
        CHECK_MSG((x % 4 == 0) && (y % 4 == 0), "Autodiff encoder requires dimensions that are multiples of 4.");

        auto extent = RHIExtent3D{static_cast<uint32_t>(x), static_cast<uint32_t>(y), 1};

        // Uncompressed source, read via texel fetch (Load) in the encoder.
        auto srcTexture = ImmediateCreateTexture(ctx.device.Get(),
            RHITextureDesc{
                .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
                .extent = extent,
                .format = RHIResourceFormat::R8G8B8A8Unorm,
                .mipLevels = 1},
            data, static_cast<size_t>(x) * y * 4, RHITextureLayout::ShaderReadOnly);
        stbi_image_free(data);

        // Decoded output, written by the encoder (storage) and sampled by the blit pass.
        auto dstTexture = ctx.device->CreateTexture(
            RHITextureDesc{
                .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::StorageImage,
                .extent = extent,
                .format = RHIResourceFormat::R8G8B8A8Unorm,
                .mipLevels = 1});

        // Live parameters, driven by the on-screen sliders below.
        float useAdam = 1.0f;     // 0 = plain SGD, 1 = Adam
        float numSteps = 30.0f;   // optimization iterations per frame
        float lr = 0.01f;          // learning rate
        float adamBeta1 = 0.9f;
        float adamBeta2 = 0.999f;
        AutodiffParams params{
            .useAdam = static_cast<int32_t>(useAdam),
            .numOptimizationSteps = static_cast<int32_t>(numSteps),
            .lr = lr,
            .adamBeta1 = adamBeta1,
            .adamBeta2 = adamBeta2,
            .textureDim = extent};

        ctx.renderer->BeginSetup();
        ResourceHandle srcHdl = ctx.renderer->CreateResource("Autodiff Source", srcTexture.Get());
        ResourceHandle dstHdl = ctx.renderer->CreateResource("Autodiff Decoded", dstTexture.Get());
        ResourceHandle sampler = ctx.renderer->CreateSampler({});

        // Compute pass: run the BC encoder / optimizer over every 4x4 tile.
        ctx.renderer->CreatePass(
            "Autodiff Encoder", RHIDeviceQueueType::Compute, 0u,
            [&](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Compute, "encoder",
                              r->GetApplication()->ResolveRelativePathBase("Data/Shaders/AutodiffSimple.spv"));
                r->BindTextureSRV(self, srcHdl, "gInputTex", RHIPipelineStageBits::ComputeShader,
                                  {.format = RHIResourceFormat::R8G8B8A8Unorm,
                                   .range = RHITextureSubresourceRange::Create()});
                r->BindTextureUAV(self, dstHdl, "gDecodedTex", RHIPipelineStageBits::ComputeShader,
                                  {.format = RHIResourceFormat::R8G8B8A8Unorm,
                                   .range = RHITextureSubresourceRange::Create()});
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(AutodiffParams));
            },
            [&, extent](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, params);
                r->CmdDispatch(self, cmd, {extent.x / 4u, extent.y / 4u, 1});
            });

        // Display the decoded result.
        createPSBackbufferBlitPass(ctx.renderer.get(), "Blit Decoded", sampler, dstHdl, RHIResourceFormat::R8G8B8A8Unorm);

        ExampleInputState input{};
        createCSDebugTextPassBackBuffer(ctx.renderer.get(), "Debug Text", Examples_HudLines(input));
        ctx.renderer->EndSetup();

        ExampleFpsCounter fps;
        while (true)
        {
            Examples_BeginFrameInput(input);
            if (Examples_PollEvents(window, ctx, input))
                break;

            Examples_BeginControls(input);
            Examples_Text(input, Format("Autodiff FPS: {}", fps.Update()));
            Examples_Slider(input, "SGD/Adam", useAdam, 0.0f, 1.0f, 1.0f);
            Examples_Slider(input, "Steps", numSteps, 1.0f, 200.0f, 1.0f);
            Examples_Slider(input, "Learning Rate", lr, 0.001f, 0.25f, 0.001f);
            if (useAdam)
            {
                Examples_Slider(input, "Adam Beta1", adamBeta1, 0.0f, 0.999f, 0.001f);
                Examples_Slider(input, "Adam Beta2", adamBeta2, 0.0f, 0.999f, 0.001f);
            }
            params.useAdam = static_cast<int32_t>(useAdam);
            params.numOptimizationSteps = static_cast<int32_t>(numSteps);
            params.lr = lr;
            params.adamBeta1 = adamBeta1;
            params.adamBeta2 = adamBeta2;
            Examples_NewFrame(window, ctx);
        }

        srcTexture.Release();
        dstTexture.Release();
    }
    Examples_DestroyVulkan(window, ctx);
    return 0;
}
