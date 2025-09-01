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
    class RHISwapchain : public RHIObject {
    protected:
        const RHIDevice& m_device;
    public:
        struct SwapchainDesc {
            enum PresentMode {
                FIFO, // V-Sync
                MAILBOX, // N buffering
                IMMEDIATE // No V-Sync (tearing allowed)
            };            
            // Name for the swap chain, used for debugging purposes.
            RHIResourceFormat format;
            // Swapchain buffer dimensions.
            RHIExtent2D dimensions;
            // Number of buffers in the swap chain. i.e. double buffering = 2, triple buffering = 3, etc.
            uint32_t buffer_count;
            // Present mode for the swap chain.
            PresentMode present_mode;
        } m_desc;
        virtual Core::StlSpan<RHITexture* const> GetImages() const = 0;
        RHISwapchain(RHIDevice const& device, SwapchainDesc const& desc) : m_device(device), m_desc(desc) {}
        /// <summary>
        /// Gets the next image in the swapchain.
        /// Raises RHISwapchainResizeException if the swapchain needs to be resized.
        /// </summary>        
        virtual uint32_t GetNextImage(
            uint64_t timeout_ns,
            RHIDeviceObjectHandle<RHIDeviceSemaphore> semaphore,
            RHIDeviceObjectHandle<RHIDeviceFence> fence
        ) = 0;            
        virtual RHIExtent2D GetDimensions() const = 0;
        inline float GetAspectRatio() const {
            auto xy = GetDimensions();
            return static_cast<float>(xy.x) / static_cast<float>(xy.y);
        }
    };
}
