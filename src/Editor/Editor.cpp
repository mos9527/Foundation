#include <Core/Logging.hpp>
#include <Platform/Application.hpp>
#include <Platform/Logging/Logging.hpp>
#include <Platform/RHI/Vulkan/VulkanApplication.hpp>
using namespace Foundation::Platform;
namespace Foundation {
    namespace Editor {
        int StartApplication(Application& app) {
            Window window = app.CreateWindow(1920, 1080, "Editor Window");
            // Vulkan init
            VulkanApplication vkApp("Editor", "Foundation", VK_API_VERSION_1_4);
            // Create a Vulkan device
            auto device = vkApp.EnumerateDevices().front();
            LOG_RUNTIME(Editor, info, "Using Vulkan Device: {}", device.lock()->GetName());
            device.lock()->Instantiate(&window);
            // Create a swap chain
            auto swapchain = device.lock()->CreateSwapchain(RHISwapchain::SwapchainDesc{
                "Editor",
                RHIResourceFormat::R8G8B8A8_UNORM,
                1920, 1080,
                3, // Triple buffering
                RHISwapchain::SwapchainDesc::PresentMode::MAILBOX
            });            
            while (!window.WindowShouldClose()) {
                // Main Loop
            }
            return 0;
        }
    }
}

int main(int argc, char** argv) {
    LOG_GET_GLOBAL_SINK()->add_sink(GetPlatformDebugLoggingSink());
    Application app(argc, argv);
    return Foundation::Editor::StartApplication(app);
}
