#pragma once

#include <Core/Core.hpp>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <stdexcept>

namespace Foundation::Native {
    class Application;
    class Window {
        friend class Application;

        GLFWwindow* m_window{ nullptr };
        Window(int width, int height, const char* title);
    public:
        Window() {};
        Window(const Window&) = delete;
        Window(Window&& other) noexcept :
            m_window(other.m_window) {
            other.m_window = nullptr;
        }
        Window& operator=(Window&& other) noexcept {
            m_window = other.m_window;
            other.m_window = nullptr;
            return *this;
        }
        ~Window();
        bool WindowShouldClose();
        GLFWwindow* GetNativeWindow() const { return m_window; }
        std::pair<int,int> GetSize() const {
            int width, height;
            glfwGetWindowSize(m_window, &width, &height);
            return { width, height };
        }
        inline constexpr operator bool() const { return m_window != nullptr; }
    };
    extern void glfw_error_callback(int error, const char* description);
    class Application {
        int m_initialized = 0;
    public:
        Window CreateWindow(int width, int height, const char* title) {
            return Window(width, height, title);
        }
        Application() {
            glfwSetErrorCallback(glfw_error_callback);
            if (!(m_initialized = glfwInit())) {
                throw std::runtime_error("Failed to initialize GLFW");
            }
        }
        ~Application() {
            if (m_initialized)
                glfwTerminate();
        }
    };
}
