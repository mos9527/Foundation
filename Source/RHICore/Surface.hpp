#pragma once
#include "Common.hpp"
#include "Swapchain.hpp" // for RHISwapchainPresentMode

namespace Foundation::RHI {
    class RHISurface : public RHIObject {
    public:
        struct SurfaceDesc {
            void* windowHandle = nullptr; // e.g. SDL_Window*
        };

        [[nodiscard]] virtual Span<RHISurfaceFormat const> GetSupportedFormats() const = 0;
        [[nodiscard]] virtual Span<RHISwapchainPresentMode const> GetSupportedPresentModes() const = 0;

        virtual void DebugSetObjectName(const char* name) = 0;
    };

}
