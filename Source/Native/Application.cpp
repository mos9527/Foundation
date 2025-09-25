#include "Application.hpp"
#include <GLFW/glfw3.h>
#include <stdexcept>
namespace Foundation::Native
{
    void glfw_error_callback(int error, const char* description)
    {
        LOG_RUNTIME(GLFW, critical, "GLFW Error ({}): {}", error, description);
    }

    NativeWindow::NativeWindow(uint32_t width, uint32_t height, const char* title) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        mWindow = glfwCreateWindow(
            static_cast<int>(width),
            static_cast<int>(height),
            title, nullptr, nullptr);
    }
    NativeWindow::~NativeWindow() {
        if (mWindow)
            glfwDestroyWindow(static_cast<GLFWwindow*>(mWindow));
    }
    Pair<uint32_t, uint32_t> NativeWindow::GetWindowSize() const
    {
        int width, height;
        glfwGetWindowSize(static_cast<GLFWwindow*>(mWindow), &width, &height);
        return { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    }
    Pair<uint32_t, uint32_t> NativeWindow::GetFramebufferSize() const
    {
        int width, height;
        glfwGetFramebufferSize(static_cast<GLFWwindow*>(mWindow), &width, &height);
        return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    }
    Pair<uint32_t, uint32_t> NativeWindow::GetWindowPosition() const
    {
        int x, y;
        glfwGetWindowPos(static_cast<GLFWwindow*>(mWindow), &x, &y);
        return {static_cast<uint32_t>(x), static_cast<uint32_t>(y)};
    }
    void NativeWindow::SetWindowTitle(const char* title) const
    {
        glfwSetWindowTitle(static_cast<GLFWwindow*>(mWindow), title);
    }
    bool NativeWindow::WindowShouldClose() const
    {
        CHECK_MSG(mWindow, "Window not initialized");
        if (!glfwWindowShouldClose(static_cast<GLFWwindow*>(mWindow))) {
            glfwPollEvents();
            return false;
        }
        else {
            return true;
        }
    }
    NativeApplication::NativeApplication() {
        glfwSetErrorCallback(glfw_error_callback);
        CHECK_MSG((mInitialized = glfwInit()) == GLFW_TRUE, "Failed to initialize GLFW");        
        mStartCounter = getPerformanceCounter();
    }
    NativeApplication::~NativeApplication() {
        if (mInitialized)
            glfwTerminate();
    }
    NativeWindow NativeApplication::CreateNativeWindow(uint32_t width, uint32_t height, const char* title) {
        return {width, height, title};
    }
}
#include <tinyfiledialogs.h>
namespace Foundation::Native {
    MessageBoxResult CreateMessageBox(const char* title, const char* message, MessageBoxType type, MessageBoxIcon icon, MessageBoxResult default_result) {
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
