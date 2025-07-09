#include <Core/Logging.hpp>
#include <Platform/Application.hpp>
#include <Platform/Logging.hpp>
#include <Renderer/VkApplication.hpp>
using namespace Foundation::Platform;
using namespace Foundation::Renderer;
namespace Foundation {
    namespace Editor {
        constexpr int s_EditorVersion = VK_MAKE_VERSION(
            FOUNDATION_EDITOR_VERSION_MAJOR,
            FOUNDATION_EDITOR_VERSION_MINOR,
            FOUNDATION_EDITOR_VERSION_PATCH
        );
        int StartApplication(Application& app) {
            Application::Window window = app.CreateWindow(1920, 1080, "Editor Window");
            // Vulkan init
            VkApplication vkApp("Editor", "Foundation", s_EditorVersion, VK_API_VERSION_1_3);
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
