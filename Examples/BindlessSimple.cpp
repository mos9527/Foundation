#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSDebugText.hpp>
#include <RenderUtils/PSFullscreen.hpp>
#include <RenderCore/Bindless.hpp>
using namespace RenderUtils;
struct PushConstant
{
    float time;
    uint32_t num_textures;
    uint32_t first_texture;
};
constexpr uint32_t kNumTextures = 64;
int main()
{
    SDL_Window* window = SDL_CreateWindow("BindlessSimple Example", 800, 600, Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, swapchain] = Examples_InitVulkan(window, {
        .enableAsyncCompute = false,
        .threads = 0, /* ST recording */
    });
    CSDebugTextData lines[5];
    lines[0].x = lines[0].y = 16, lines[0].SetText("Bindless Simple");
    {
        BindlessPool bindings(device.Get(), GLOBAL_ALLOC, {.maxBindings = kNumTextures });
        Vector<RHIDeviceScopedObjectHandle<RHITexture>> textures(kNumTextures, GLOBAL_ALLOC);
        // Prepare textures
        for (auto& tex : textures)
        {
            const auto format = RHIResourceFormat::R8G8B8A8Unorm;
            const auto extent = RHIExtent3D{ 256, 256, 1 };
            tex = device->CreateTexture({
                .resource = { .heap = RHIDeviceHeapType::Upload, .hostAccess = RHIResourceHostAccess::WriteOnly },
                .usage = RHITextureUsageBits::SampledImage,
                .extent = extent,
                .format = format,
            });
            auto view = tex->CreateTextureView(RHITextureViewDesc{
                .format = format,
                .range = RHITextureSubresourceRange::Create()
            });
            auto pixels = Span<uint32_t>(static_cast<uint32_t*>(tex->Map()), extent.x * extent.y);
            // Fill with a color pattern
            uint32_t color = 0xFF000000 | ((rand() % 256) << 16) | ((rand() % 256) << 8) | (rand() % 256);
            for (size_t i = 0; i < pixels.size(); ++i)
                pixels[i] = color;
            bindings.Allocate(view.Release().Get()); // OK to release - textures own the view.
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
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, PushConstant{
                    .time = Examples_GetTime(),
                    .num_textures = kNumTextures,
                    .first_texture = 0
                });
            }
        );
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
