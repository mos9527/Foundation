#include "Application.hpp"
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <stdexcept>
namespace Foundation::Native {
    void glfw_error_callback(int error, const char* description)
    {
        LOG_RUNTIME(GLFW, critical, "GLFW Error ({}): {}", error, description);
    }

    Window::Window(int width, int height, const char* title) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        m_window = glfwCreateWindow(width, height, title, NULL, NULL);
    }
    Window::~Window() {
        if (m_window)
            glfwDestroyWindow((GLFWwindow*)m_window);
    }
    std::pair<uint32_t, uint32_t> Window::GetSize() const {
        int width, height;
        glfwGetWindowSize((GLFWwindow*)m_window, &width, &height);
        return { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    }
    bool Window::WindowShouldClose() {
        CHECK_MSG(m_window, "Window not initalized");
        if (!glfwWindowShouldClose((GLFWwindow*)m_window)) {
            glfwPollEvents();
            return false;
        }
        else {
            return true;
        }
    }
    Application::Application() {        
        glfwSetErrorCallback(glfw_error_callback);
        CHECK_MSG((m_initialized = glfwInit()) == GLFW_TRUE, "Failed to initialize GLFW");
        m_startCounter = getPerformanceCounter();
    }
    Application::~Application() {
        if (m_initialized)
            glfwTerminate();
    }
    Window Application::CreateWindow(int width, int height, const char* title) {
        return Window(width, height, title);
    }
}
