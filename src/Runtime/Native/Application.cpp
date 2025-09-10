#include "Application.hpp"
#include <GLFW/glfw3.h>
#include <tinyfiledialogs.h>
#include <stdexcept>
namespace Foundation::Native {
    void glfw_error_callback(int error, const char* description)
    {
        LOG_RUNTIME(GLFW, critical, "GLFW Error ({}): {}", error, description);
    }

    NativeWindow::NativeWindow(uint32_t width, uint32_t height, const char* title) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        m_window = glfwCreateWindow(
            static_cast<int>(width),
            static_cast<int>(height),
            title, nullptr, nullptr);
    }
    NativeWindow::~NativeWindow() {
        if (m_window)
            glfwDestroyWindow(static_cast<GLFWwindow*>(m_window));
    }
    Pair<uint32_t, uint32_t> NativeWindow::GetWindowSize() const
    {
        int width, height;
        glfwGetWindowSize(static_cast<GLFWwindow*>(m_window), &width, &height);
        return { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    }
    Pair<uint32_t, uint32_t> NativeWindow::GetFramebufferSize() const
    {
        int width, height;
        glfwGetFramebufferSize(static_cast<GLFWwindow*>(m_window), &width, &height);
        return { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    }
    void NativeWindow::SetWindowTitle(const char* title) const
    {
        glfwSetWindowTitle(static_cast<GLFWwindow*>(m_window), title);
    }
    bool NativeWindow::WindowShouldClose() const
    {
        CHECK_MSG(m_window, "Window not initialized");
        if (!glfwWindowShouldClose(static_cast<GLFWwindow*>(m_window))) {
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
    NativeWindow NativeApplication::CreateWindow(uint32_t width, uint32_t height, const char* title) {
        return {width, height, title};
    }
    MessageBoxResult NativeApplication::MessageBox(const char* title, const char* message, MessageBoxType type, MessageBoxIcon icon, MessageBoxResult default_result) {
        const char* kDialogueType[] = { "ok", "okcancel", "yesno", "yesnocancel" };
        const char* kIconType[] = { "info", "warning", "error", "question" };
        return static_cast<MessageBoxResult>(tinyfd_messageBox(
            title, message,
            kDialogueType[static_cast<int>(type)],
            kIconType[static_cast<int>(icon)],
            static_cast<int>(default_result)
        ));
    }
}
