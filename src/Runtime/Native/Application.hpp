#pragma once
#include <Core/Core.hpp>
#include <Bits/Chrono.hpp>

namespace Foundation::Native {
    class Application;
    class Window {
        friend class Application;
        void* m_window{ nullptr };
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

        std::pair<uint32_t, uint32_t> GetSize() const;
        bool WindowShouldClose();

        inline void* GetNative() const { return m_window; }
        inline constexpr operator bool() const { return m_window != nullptr; }
    };
    class Application {
        int m_initialized = 0;
        size_t m_startCounter = 0;
    public:
        /**
         * @brief Creates a message box with the specified title and message.
         *
         * This is blocking, and will halt execution until the user dismisses it.       
         */
        void MessageBox(const char* title, const char* message);
        /**
         * @brief Creates a window with the specified width, height, and title.
         */
        Window CreateWindow(int width, int height, const char* title);
        /**
         * @brief Returns a high-resolution time in seconds since the application started.
         */
        template<typename T = float> T GetApplicationTime() const { return (getPerformanceCounter() - m_startCounter) / 1e9; }
        /**
         * @brief Returns the time in seconds since the Unix epoch (1970-01-01 00:00:00 UTC).
         *
         * This is generally not useful for measuring time intervals, as it's not
         * monotonic and can be affected by system clock changes.
         *
         * It's advised to use GetApplicationTime() for such purposes instead.        
         */
        template<typename T = float> T GetSystemTime() const { return (getEpochTime() - m_startCounter) / 1e9; }
        Application();
        ~Application();
    };
}
