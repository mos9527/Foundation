#pragma once
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <stdexcept>

namespace Foundation {
    namespace Platform {
        class Application {
            int m_initialized = 0;
        public:
            const int m_argc;
            const char** m_argv;
            class Window {
                friend class Application;

                GLFWwindow* m_window;
                Window(int width, int height, const char* title);			
            public:
                Window(const Window&) = delete;
                Window(Window&& other) noexcept : m_window(other.m_window) {
                    other.m_window = nullptr;
                }
                ~Window();
                bool WindowShouldClose();
            };

            Window CreateWindow(int width, int height, const char* title) {
                return Window(width, height, title);
            }
            Application(int argc, char** argv) : m_argc(argc), m_argv(const_cast<const char**>(argv)) {
                if (!(m_initialized = glfwInit())) {
                    throw std::runtime_error("Failed to initialize GLFW");
                }
            }
            ~Application() {
                if (m_initialized)
                    glfwTerminate();
            }
        };

    };
}
