#include "Application.hpp"
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <tinyfiledialogs.h>
#include <stdexcept>
namespace Foundation::Native {
    void glfw_error_callback(int error, const char* description)
    {
        LOG_RUNTIME(GLFW, critical, "GLFW Error ({}): {}", error, description);
    }

    NativeWindow::NativeWindow(int width, int height, const char* title) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        m_window = glfwCreateWindow(width, height, title, NULL, NULL);
    }
    NativeWindow::~NativeWindow() {
        if (m_window)
            glfwDestroyWindow((GLFWwindow*)m_window);
    }
    std::pair<uint32_t, uint32_t> NativeWindow::GetWindowSize() const
    {
        int width, height;
        glfwGetWindowSize((GLFWwindow*)m_window, &width, &height);
        return { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    }
    std::pair<uint32_t, uint32_t> NativeWindow::GetFramebufferSize() const
    {
        int width, height;
        glfwGetFramebufferSize((GLFWwindow*)m_window, &width, &height);
        return { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    }
    void NativeWindow::SetWindowTitle(const char* title)
    {
        glfwSetWindowTitle((GLFWwindow*)m_window, title);
    }
    bool NativeWindow::WindowShouldClose() {
        CHECK_MSG(m_window, "Window not initalized");
        if (!glfwWindowShouldClose((GLFWwindow*)m_window)) {
            glfwPollEvents();
            return false;
        }
        else {
            return true;
        }
    }
    NativeApplication::NativeApplication() {
        glfwSetErrorCallback(glfw_error_callback);
        CHECK_MSG((m_initialized = glfwInit()) == GLFW_TRUE, "Failed to initialize GLFW");        
        m_startCounter = getPerformanceCounter();
    }
    NativeApplication::~NativeApplication() {
        if (m_initialized)
            glfwTerminate();
    }
    NativeWindow NativeApplication::CreateWindow(int width, int height, const char* title) {
        return NativeWindow(width, height, title);
    }
    MessageBoxResult NativeApplication::MessageBox(const char* title, const char* message, MessageBoxType type, MessageBoxIcon icon, MessageBoxResult default_result) {
        const char* kDialougeType[] = { "ok", "okcancel", "yesno", "yesnocancel" };
        const char* kIconType[] = { "info", "warning", "error", "question" };
        return (MessageBoxResult)tinyfd_messageBox(title, message, kDialougeType[(int)type], kIconType[(int)icon], (int)default_result);
    }
}
