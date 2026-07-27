// Demonstrates bindless texture sampling with a pool of procedurally generated textures.
// The fullscreen shader cycles through many sampled images without rebinding descriptors.
#include "Examples.hpp"
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
int main(int argc, char** argv)
{
    SDL_Window* window = SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("BindlessSimple Example"), 800, 600,
                                          Examples_SDLWindowFlagsVulkan);
    auto ctx =
        Examples_InitVulkan(window, argc, argv,
                            {
                                .asyncCompute = false, .threadCount = 0, /* ST recording */
                            });
    CSDebugTextData lines[5]{};
    lines[0].x = lines[0].y = 16, lines[0].SetText("Bindless Simple");
    {
        BindlessPool bindings(ctx.device.Get(), GLOBAL_ALLOC, {.maxBindings = kNumTextures});
        Vector<RHIDeviceScopedHandle<RHITexture>> textures(kNumTextures, GLOBAL_ALLOC);
        // Prepare textures
        {
            const auto format = RHIResourceFormat::R8G8B8A8Unorm;
            const auto extent = RHIExtent3D{256, 256, 1};
            Vector<uint32_t> pattern(extent.x * extent.y, GLOBAL_ALLOC);
            for (auto& tex : textures)
            {
                // Fill with a color pattern
                uint32_t color = 0xFF000000 | ((rand() % 256) << 16) | ((rand() % 256) << 8) | (rand() % 256);
                for (uint32_t i = 0; i < pattern.size(); ++i)
                    pattern[i] = color;
                tex = ImmediateCreateTexture(
                    ctx.device.Get(),
                    RHITextureDesc{
                        .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
                        .extent = extent,
                        .format = format,
                    },
                    pattern.data(), pattern.size() * sizeof(uint32_t),
                    RHITextureLayout::ShaderReadOnly);
                auto view = tex->CreateTextureView(
                    RHITextureViewDesc{.format = format, .range = RHITextureSubresourceRange::Create()});
                bindings.Allocate(view.Release().Get()); // OK to release - textures own the view.
            }
        }
        ctx.renderer->BeginSetup();
        ResourceHandle linSampler = ctx.renderer->CreateSampler({});
        createPSFullscreenPass(
            ctx.renderer.get(), "Atlas Display",
            [&](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", r->GetApplication()->ResolveRelativePathBase("Data/Shaders/BindlessSimple.spv"));
                r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(PushConstant));
                r->BindTextureSampler(self, linSampler, "sampler");
                r->BindDescriptorSet(self, "gTextures2D",  bindings.GetDescriptorSetLayout());
            },
            [&](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                r->CmdBindDescriptorSet(self, cmd, "gTextures2D", bindings.GetDescriptorSet());
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0,
                                      PushConstant{.time = Examples_GetTime(), .total = kNumTextures, .first = 0});
            });
        createCSDebugTextPassBackBuffer(ctx.renderer.get(), "Debug Text", lines);
        ctx.renderer->EndSetup();

        ExampleFpsCounter fps;
        while (!Examples_ShouldClose(window, ctx))
        {
            lines[1].x = 16, lines[1].y = 40, lines[1].SetText(Format("FPS: {}", fps.Update()));
            Examples_NewFrame(window, ctx);
        }
    }
    Examples_DestroyVulkan(window, ctx);
    return 0;
}
