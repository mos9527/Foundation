#include <Platform/Application.hpp>
#include <Core/Logging.hpp>
namespace Foundation {
    namespace Platform {
        void glfw_error_callback(int error, const char* description)
        {
            LOG_RUNTIME(GLFW, critical, "GLFW Error: %s", description);
            Core::BugCheck("GLFW Error");
        }

        Window::Window(int width, int height, const char* title) {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            m_window = glfwCreateWindow(width, height, title, NULL, NULL);
        }
        Window::~Window() {
            glfwDestroyWindow(m_window);
        }
        bool Window::WindowShouldClose() {
            if (!glfwWindowShouldClose(m_window)) {
                glfwPollEvents();
                return false;
            } else {
                return true;
            }
        }
    }
}
