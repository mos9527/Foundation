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
    g_renderer->CreatePass<ScenePass>("Scene Data", RHIDevicePipelineType::Graphics, *g_renderer,  *g_scene);
    g_renderer->EndSetup(0);
}

void createResources() {
    CHECK(g_device);
    g_device->WaitIdle();
    g_swapchain.Reset();
    g_swapchain = g_device->CreateSwapchain(RHISwapchain::SwapchainDesc{
        .format = RHIResourceFormat::R8G8B8A8_UNORM,
        .extents = getWindowSize(),
        .buffer_count = 3,
        .present_mode = RHISwapchain::SwapchainDesc::PresentMode::MAILBOX,
    });
    g_renderer = ConstructUnique<Renderer>(g_alloc.Ptr(), g_device, g_swapchain, g_alloc_renderer.Ptr());
    g_scene = ConstructUnique<Scene>(g_alloc.Ptr(), g_alloc_scene.Ptr(), *g_renderer, SceneDataDesc{});
}

int main(int argc, char** argv) {
    g_window = g_app.CreateWindow(1920, 1080, "Viewport");
    VulkanApplication vulkan("ModelViewer", "ModelViewer", VK_API_VERSION_1_3, g_window, g_alloc.Ptr());
    g_device = vulkan.CreateDevice(vulkan.EnumerateDevices()[0]);
    createResources();
    createRenderPasses();
    LOG_RUNTIME(ModelViewer, info, "Entering main loop");
    LOG_RUNTIME(ModelViewer, info, "Main Allocation: {}", format_as_readable_size(g_alloc.GetUsedMemory()));
    LOG_RUNTIME(ModelViewer, info, "Render Allocation: {}", format_as_readable_size(g_alloc_renderer.GetUsedMemory()));
    LOG_RUNTIME(ModelViewer, info, "Scene Allocation: {}", format_as_readable_size(g_alloc_scene.GetUsedMemory()));
    while (!g_window.WindowShouldClose()) {
        g_renderer->Execute();
    }
}
