#include <Core/Logging.hpp>
#include <Core/Bits/StringUtils.hpp>
#include <Core/Allocator/MimallocAllocator.hpp>
#include <Platform/Application.hpp>
#include <Platform/RHI/Swapchain.hpp>
#include <Platform/RHI/Vulkan/Application.hpp>
using namespace Foundation::Core;
using namespace Foundation::Platform;
using namespace Foundation::Platform::RHI;
namespace Foundation {
    namespace Editor {
        Core::MimallocAllocator g_Allocator;
        int StartApplication(Application& app) {
            Window window = app.CreateWindow(1920, 1080, "Editor Window");
            // Vulkan init
            VulkanApplication vkApp("Editor", "Foundation", VK_API_VERSION_1_4, &g_Allocator);
            // Create a Vulkan device
            auto device_desc = vkApp.EnumerateDevices().front();
            LOG_RUNTIME(Editor, info, "Using Vulkan Device: {}", device_desc.name);
            device_desc.SetWindow(&window);
            auto device = vkApp.CreateDevice(device_desc);
            // Create a Swapchain
            device.Get()->CreateSwapchain(RHISwapchain::SwapchainDesc{
                .name = "Editor Swapchain",
                .format = RHIResourceFormat::R8G8B8A8_UNORM,
                .width = 1920,
                .height = 1080,
                .buffer_count = 3,
                .present_mode = RHISwapchain::SwapchainDesc::PresentMode::MAILBOX
                }
            );
            LOG_RUNTIME(Editor, info, "Memory Tracked: {}", Bits::ByteSizeToString(g_Allocator.GetAllocatedSize()));
            while (!window.WindowShouldClose()) {
                // Main Loop
            }
            return 0;
        }
    }
}

int main(int argc, char** argv) {
    try {
        Application app(argc, argv);
        return Foundation::Editor::StartApplication(app);
    }
    catch (const std::exception& e) {
        BugCheck(e);
    }
}
