#pragma once
#include <Core/Core.hpp>
#include <Bits/Chrono.hpp>
#include "Enums.hpp"
/**
 * @brief Platform-dependent native application abstractions
 */
namespace Foundation::Native {
    using namespace Core;
    class Application;
    /**
     * @brief Class representing a window for the platform.
     */
    class NativeWindow {
        friend class NativeApplication;
        void* m_window{ nullptr };
        NativeWindow(uint32_t width, uint32_t height, const char* title);
    public:
        NativeWindow() = default;
        NativeWindow(const NativeWindow&) = delete;
        NativeWindow(NativeWindow&& other) noexcept :
            m_window(other.m_window) {
            other.m_window = nullptr;
        }
        NativeWindow& operator=(NativeWindow&& other) noexcept {
            m_window = other.m_window;
            other.m_window = nullptr;
            return *this;
        }
        ~NativeWindow();

        [[nodiscard]] Pair<uint32_t, uint32_t> GetWindowSize() const;
        [[nodiscard]] Pair<uint32_t, uint32_t> GetFramebufferSize() const;
        void SetWindowTitle(const char* title) const;

        [[nodiscard]] bool WindowShouldClose() const;

        [[nodiscard]] void* GetNative() const { return m_window; }
        constexpr explicit operator bool() const { return m_window != nullptr; }
    };

    /**
     * @brief Application base class.
     * Handles initialization and shutdown of the native platform, and windowing management.
     */
    class NativeApplication {
        int m_initialized = 0;
        size_t m_startCounter = 0;
    public:
        /**
         * @brief Creates a window with the specified width, height, and title.
         */
        [[nodiscard]] static NativeWindow CreateWindow(uint32_t width, uint32_t height, const char* title);
        /**
         * @brief Returns a high-resolution time in seconds since the application started.
         * @tparam T The return type. Must be a floating-point type. Default is float.
         */
        template<typename T = float> T GetApplicationTime() const { return (getPerformanceCounter() - m_startCounter) / 1e9; }
        /**
         * @brief Returns a high-resolution time in _nanoseconds_ since the application started.
         */
        size_t GetApplicationCounter() const { return getPerformanceCounter() - m_startCounter; }
        NativeApplication();
        virtual ~NativeApplication();
    };
    /**
    * @brief Creates a message box with the specified title and message.
    *
    * This is blocking, and will halt execution until the user dismisses it.
    */
    MessageBoxResult MessageBox(
        const char* title, const char* message,
        MessageBoxType type = MessageBoxType::Ok,
        MessageBoxIcon icon = MessageBoxIcon::Info,
        MessageBoxResult default_result = MessageBoxResult::Yes
    );
}
