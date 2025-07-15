#include <Core/Logging.hpp>
#include <Core/Bits/StringUtils.hpp>
#include <Platform/Application.hpp>
#include <Platform/RHI/Swapchain.hpp>
#include <Platform/RHI/Vulkan/Application.hpp>
#include <Core/Allocator/HeapAllocator.hpp>
#include <Renderer/Renderer.hpp>
using namespace Foundation::Core;
using namespace Foundation::Platform;
using namespace Foundation::Platform::RHI;
namespace Foundation {
    namespace Editor {
        Core::HeapAllocatorMultiThreaded g_Allocator;
        int StartApplication(Application& app) {
            {
                VulkanApplication vkApp("Editor", "Foundation", VK_API_VERSION_1_4, &g_Allocator);
                Window window = app.CreateWindow(1920, 1080, "Editor Window");
                auto device = vkApp.CreateDevice(vkApp.EnumerateDevices()[0], &window);
                Renderer::Renderer renderer(device.Get(), g_Allocator.Ptr());
                while (!window.WindowShouldClose()) {
                    // Main Loop
                    renderer.Draw();
                }
            }
            LOG_RUNTIME(Editor, info, "Quitting. Memory Used: {}", Bits::ByteSizeToString(g_Allocator.GetUsedMemory()));
            return 0;
        }
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
    return Foundation::Editor::StartApplication(app);
#endif
}
