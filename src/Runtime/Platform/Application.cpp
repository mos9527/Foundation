#include "Application.hpp"
#include "Logging.hpp"
#include <spdlog/sinks/stdout_color_sinks.h>

namespace Foundation::Core {
    spdlog::sink_ptr GetPlatformDebugLoggingSink() {
        return std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    }

    void ExceptionHandler(const std::exception& e, std::vector<std::string> backtrace) {
        printf("*** RUNTIME EXCEPTION ***\n");
        printf("%s\n", e.what());
        printf("*** BACKTRACE ***\n");
        for (const auto& frame : backtrace)
            printf("%s\n", frame.c_str());
        exit(-1);
    }

    void glfw_error_callback(int error, const char* description)
    {
        throw std::runtime_error(description);
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
        }
        else {
            return true;
        }
    }
}
