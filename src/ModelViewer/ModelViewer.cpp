/*
TODOs:
- Validation layers is very unhappy about how we're transitioning swapchain backbuffers
- No swap chain resize
- Resources modifed in flight should be double buffered
- No proper compute pipeline creation API yet
*/
#include "ModelViewer.hpp"

Native::Application g_app;
Native::Window g_window;
DefaultAllocator g_alloc, g_alloc_renderer, g_alloc_scene;
RHIApplicationScopedObjectHandle<RHIDevice> g_device;
RHIDeviceScopedObjectHandle<RHISwapchain> g_swapchain;
UniquePtr<Renderer> g_renderer;
UniquePtr<Scene> g_scene;

inline const RHIExtent2D getWindowSize() { auto [w, h] = g_window.GetSize(); return { w, h }; }

void createRenderPasses() {
    CHECK(g_renderer && g_scene);
    g_renderer->BeginSetup();
    ResourceHandle s_global, s_instance, s_primitive, gb_albedo, cp_sampler;
    gb_albedo = createResource(
        g_renderer.get(),
        "GBuffer Albedo",
        RHITextureDesc{
            .usage = RHITextureUsageBits::RenderTarget | RHITextureUsageBits::SampledImage,
            .extent = g_renderer->GetSwapchainExtent3D(),
            .format = RHIResourceFormat::R8G8B8A8_UNORM
        });
    cp_sampler = createSampler(g_renderer.get(), "Copy Sampler", {});
    createSceneUpdatePass(g_renderer.get(), g_scene.get(), s_global, s_instance, s_primitive);
    createPass(
        g_renderer.get(), "GBuffer",
        RHIDevicePipelineType::Graphics,
        [=](PassHandle self, Renderer* r) {
            r->BindBufferShaderRead(self, s_primitive);
            r->BindBufferShaderRead(self, s_instance);
            r->BindTextureRTV(self, gb_albedo, { .format = RHIResourceFormat::R8G8B8A8_UNORM });
            r->BindShader(self, RHIShaderStageBits::Vertex, ".derived/shaders/GBuffer_vertMain.spirv");
            r->BindShader(self, RHIShaderStageBits::Fragment, ".derived/shaders/GBuffer_fragMain.spirv");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd) {
            auto const& img_wh = r->GetSwapchainExtent();
            r->CmdBeginGraphics(self, cmd, r->GetSwapchainExtent());
            r->CmdSetPipeline(self, cmd);
            cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                .SetScissor(0, 0, img_wh.x, img_wh.y);
            cmd->Draw(3); // Another triangle...sigh
            cmd->EndGraphics();
        }
    );
    createPSBackbufferBlitPass(g_renderer.get(), "Backbuffer Blit", cp_sampler, gb_albedo);
    g_renderer->EndSetup();
}

void createResources() {
    CHECK(g_device);
    g_device->WaitIdle();
    g_swapchain.Reset();
    g_swapchain = g_device->CreateSwapchain(
        RHISwapchain::SwapchainDesc{
        .format = RHIResourceFormat::R8G8B8A8_UNORM,
        .extents = getWindowSize(),
        .buffer_count = 3,
        .present_mode = RHISwapchain::SwapchainDesc::PresentMode::MAILBOX,
        });
    g_renderer = ConstructUnique<Renderer>(g_alloc_renderer.Ptr(), g_device, g_swapchain, g_alloc_renderer.Ptr());
    g_scene = ConstructUnique<Scene>(g_alloc_scene.Ptr(), g_alloc_scene.Ptr(), g_device.Get(), SceneDataDesc{});
}

int main(int argc, char** argv) {
    g_window = g_app.CreateWindow(1920, 1080, "Viewport");
    VulkanApplication vulkan("ModelViewer", "ModelViewer", VK_API_VERSION_1_3, g_window, g_alloc.Ptr());
    g_device = vulkan.CreateDevice(vulkan.EnumerateDevices()[0]);
    createResources();
    createRenderPasses();
    while (!g_window.WindowShouldClose()) {
        g_renderer->Execute();
    }
    // The existence of
    // Static Initialization Fiasco
    // Implies the existence of
    // Static Deinitialization Fiasco
    // This is not a haiku
    // Nor something you should see
    // in production code
    g_device->WaitIdle();
    g_scene.reset();
    g_renderer.reset();
    g_swapchain.Reset();
    g_device.Reset();
}
