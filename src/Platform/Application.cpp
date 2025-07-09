#include <Platform/Application.hpp>

namespace Foundation {
    namespace Platform {	
        Application::Window::Window(int width, int height, const char* title) {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            m_window = glfwCreateWindow(width, height, title, NULL, NULL);
        }
        Application::Window::~Window() {
            glfwDestroyWindow(m_window);
        }
        bool Application::Window::WindowShouldClose() {
            if (!glfwWindowShouldClose(m_window)) {
                glfwPollEvents();
                return false;
            } else {
                return true;
            }
        }
    }
}
