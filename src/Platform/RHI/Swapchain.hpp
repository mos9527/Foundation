#pragma once
#include <Platform/RHI/Common.hpp>

namespace Foundation {
    namespace Platform {
        namespace RHI {
            class RHIDevice;
            class RHIImage;
            class RHIDeviceSemaphore;
            class RHIDeviceFence;
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
                    std::string name;
                    // Name for the swap chain, used for debugging purposes.
                    RHIResourceFormat format;
                    // Swapchain buffer dimensions.
                    RHIExtent2D dimensions;
                    // Number of buffers in the swap chain. i.e. double buffering = 2, triple buffering = 3, etc.
                    uint32_t buffer_count;
                    // Present mode for the swap chain.
                    PresentMode present_mode;
                } const m_desc;
                
                virtual Core::StlSpan<RHIImage* const> GetImages() const = 0;
                RHISwapchain(RHIDevice const& device, SwapchainDesc const& desc) : m_device(device), m_desc(desc) {}
                virtual size_t GetNextImage(
                    uint64_t timeout_ns,
                    RHIDeviceObjectHandle<RHIDeviceSemaphore> semaphore,
                    RHIDeviceObjectHandle<RHIDeviceFence> fence
                ) = 0;
                /// <summary>
                /// Retrives the [width, height] dimensions of the swapchain images.
                /// </summary>                
                virtual std::pair<size_t, size_t> GetDimensions() const = 0;
                inline float GetAspectRatio() const {
                    auto [width, height] = GetDimensions();
                    return static_cast<float>(width) / static_cast<float>(height);
                }
            };
        }
    }
}
