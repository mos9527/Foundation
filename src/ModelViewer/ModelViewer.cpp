#include <Bits/StringUtils.hpp>

#include <Core/Platform/Logging.hpp>
#include <Core/Platform/Application.hpp>
#include <Core/Allocator/DefaultAllocator.hpp>

#include <RHICore/Swapchain.hpp>
#include <RHIVulkan/Application.hpp>

#include <Renderer/Renderer.hpp>
#include <Renderer/GBuffer.hpp>
#include <Renderer/Scene.hpp>

using namespace Foundation::Core;
using namespace Foundation::RHI;
namespace Foundation {
    class Editor {
        Allocator* alloc;
        RHIApplication* app;
        Window& window;

        RHIApplicationScopedObjectHandle<RHIDevice> m_device;
        RHIDeviceScopedObjectHandle<RHISwapchain> m_swapchain;

        UniquePtr<Renderer> m_renderer;
        UniquePtr<Scene> m_scene;
        void CreateSwapchain(RHIExtent2D size) {
            m_device->WaitIdle();
            m_swapchain.Reset();
            m_swapchain = m_device->CreateSwapchain(RHISwapchain::SwapchainDesc{
                .format = RHIResourceFormat::R8G8B8A8_UNORM,
                .extents = size,
                .buffer_count = 3,
                .present_mode = RHISwapchain::SwapchainDesc::PresentMode::MAILBOX,
            });
        }
        void CreateResources(RHIExtent2D swapchainSize) {
            CreateSwapchain(swapchainSize);
            m_renderer = ConstructUnique<Renderer>(alloc, m_device, m_swapchain, alloc);
            m_scene    = ConstructUnique<Scene>(alloc, alloc, *m_renderer, SceneDataDesc{
                .PrimitiveDataBudget = 256 * (1 << 20LL),
                .InstanceDataBudget =  64 * (1 << 20LL),
                .GlobalDataBudget = 1 * (1 << 20LL),
            });
            m_renderer->BeginSetup();
            auto [hScene, scene] = m_renderer->CreatePass<ScenePass>({ .name = "Scene Data Transfer" }, *m_renderer, *m_scene);
            auto [hgbuffer, gbuffer] = m_renderer->CreatePass<GBuffer>(
                { .name = "GBuffer", .type = RHIDevicePipelineType::Graphics },
                *m_renderer, scene->m_global, scene->m_instance, scene->m_primitive
            );
            m_renderer->EndSetup(hgbuffer);
        }

        RHIExtent2D GetWindowSize() const {
            auto [w, h] = window.GetSize();
            return { w, h };
        }
        
    public:
        Editor(Allocator* alloc, RHIApplication* app, Window& window) : alloc(alloc), app(app), window(window) {
            m_device = app->CreateDevice(app->EnumerateDevices()[0], &window);
            CreateResources(GetWindowSize());
        }

        void Run() {
            if (m_swapchain->GetExtents() != GetWindowSize())
                CreateResources(GetWindowSize());
            m_renderer->Execute();
        }
    };

    int StartApplication(Application& app) {
        DefaultAllocator g_global, g_render;
        {
            Window window = app.CreateWindow(1920, 1080, "Editor Window");
            VulkanApplication vulkan("Editor", "Foundation", VK_API_VERSION_1_3, &g_global);
            Editor editor(&g_render, &vulkan, window);
            while (!window.WindowShouldClose()) {
                editor.Run();
            }
        }
        LOG_RUNTIME(Editor, info, "Quitting. Memory Used: {}", Bits::ByteSizeToString(g_global.GetUsedMemory()));
        return 0;
    }
}

int main(int argc, char** argv) {
    Application app(argc, argv);
#ifndef _DEBUG
    try {
        return Foundation::Editor::StartApplication(app);
    }
    catch (const std::exception& e) {
        BugCheck(e);

    }
#else
    return Foundation::StartApplication(app);
#endif
}
