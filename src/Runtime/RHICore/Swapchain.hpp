#pragma once
#include "Common.hpp"
namespace Foundation::RHI {
    class RHIDevice;
    class RHITexture;
    class RHIDeviceSemaphore;
    class RHIDeviceFence;
    struct RHISwapchainResizeException : std::exception {
        using std::exception::exception;
    };
    enum class RHISwapchainPresentMode {
        // V-Sync
        Fifo,
        // N buffering
        Mailbox,
        // No V-Sync (tearing allowed)
        Tearing
    };
    class RHISwapchain : public RHIObject {
    protected:
        const RHIDevice& m_device;
    public:
        struct SwapchainDesc {
            // Name for the swap chain, used for debugging purposes.
            RHIResourceFormat format;
            // Swapchain buffer sizes.
            RHIExtent2D extents;
            // Min number of buffers in the swap chain. i.e. double buffering = 2, triple buffering = 3, etc.
            // Driver may create more buffers than requested.
            uint32_t min_buffer_count;
            // Present mode for the swap chain.
            RHISwapchainPresentMode present_mode;
        } m_desc;
        virtual Core::Span<RHITexture* const> GetImages() const = 0;
        RHISwapchain(RHIDevice const& device, SwapchainDesc const& desc) : m_device(device), m_desc(desc) {}
        /**
         * @brief Gets the next image in the swapchain.
         * Raises RHISwapchainResizeException if the swapchain needs to be resized.
         */
        virtual uint32_t GetNextImage(
            uint64_t timeout_ns,
            RHIDeviceObjectHandle<RHIDeviceSemaphore> semaphore,
            RHIDeviceObjectHandle<RHIDeviceFence> fence
        ) = 0;            
        virtual RHIExtent2D GetExtents() const = 0;
        inline float GetAspectRatio() const {
            auto xy = GetExtents();
            return static_cast<float>(xy.x) / static_cast<float>(xy.y);
        }

        virtual void DebugSetObjectName(const char* name) = 0;
    };
    ENUM_NAME_CONV_BEGIN(RHISwapchainPresentMode)
        ENUM_NAME(Fifo)
        ENUM_NAME(Mailbox)
        ENUM_NAME(Tearing)
    ENUM_NAME_CONV_END()
}
