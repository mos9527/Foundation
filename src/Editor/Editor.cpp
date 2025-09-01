#include <Core/Platform/Logging.hpp>
#include <Bits/StringUtils.hpp>
#include <Core/Platform/Application.hpp>
#include <RHICore/Swapchain.hpp>
#include <RHIVulkan/Application.hpp>
#include <Core/Allocator/HeapAllocator.hpp>
#include <Renderer/Renderer.hpp>

using namespace Foundation::Core;
using namespace Foundation::RHI;
namespace Foundation {
    Core::HeapAllocatorMultiThreaded g_Allocator;
    void SetupRenderer(Renderer& renderer) {
        renderer.BeginSetup();
        // renderer.EndSetup();
    }

    int StartApplication(Application& app) {
        {            
            VulkanApplication vkApp("Editor", "Foundation", VK_API_VERSION_1_3, &g_Allocator);
            Window window = app.CreateWindow(1920, 1080, "Editor Window");
            auto device = vkApp.CreateDevice(vkApp.EnumerateDevices()[0], &window);
            Renderer renderer(device, RHIExtent2D(window.GetSize().first, window.GetSize().second), g_Allocator.Ptr());
            SetupRenderer(renderer);
            while (!window.WindowShouldClose()) {
                // Main Loop
                renderer.Execute(RHIExtent2D(window.GetSize().first, window.GetSize().second));
            }
        }
        LOG_RUNTIME(Editor, info, "Quitting. Memory Used: {}", Bits::ByteSizeToString(g_Allocator.GetUsedMemory()));
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
