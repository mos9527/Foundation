#pragma once
#include <Platform/Application.hpp>
#include <vector>
#include <memory>
#include <span>

namespace Foundation {
    namespace Platform {
        enum class RHIResourceFormat {
            Undefined = 0,
            R8G8B8A8_UNORM,       
        };
    }
}
namespace Foundation {
    namespace Platform {
        class RHIObject {
        public:
            RHIObject() = default;
            RHIObject(RHIObject const&) = delete;
            RHIObject& operator=(const RHIObject&) = delete;

            virtual const char* GetName() const = 0;
        };
        class RHIApplication;
        class RHIDevice;
        class RHISwapchain : public RHIObject {
            const RHIDevice& m_device;
        public:
            struct SwapchainDesc {
                enum PresentMode {
                    FIFO, // V-Sync
                    MAILBOX, // Triple buffering
                    IMMEDIATE // No V-Sync (tearing allowed)
                };
                std::string name;
                // Name for the swap chain, used for debugging purposes.
                RHIResourceFormat format;
                // Swapchain buffer dimensions.
                uint32_t width, height;
                // Number of buffers in the swap chain. i.e. double buffering = 2, triple buffering = 3, etc.
                uint32_t buffer_count;
                // Present mode for the swap chain.
                PresentMode present_mode;
            };
            const SwapchainDesc m_desc;
            
            virtual bool IsValid() const = 0;

            RHISwapchain(RHIDevice const& device, SwapchainDesc const& desc) : m_device(device), m_desc(desc) {}
        };
        class RHIDevice : public RHIObject {
            const RHIApplication& m_app;
        public:
            RHIDevice(RHIApplication const& app) : m_app(app) {}
            RHIDevice(RHIDevice const&) = delete;

            virtual void Instantiate(Window*) = 0;
            virtual bool IsValid() const = 0;

            virtual std::unique_ptr<RHISwapchain> CreateSwapchain(RHISwapchain::SwapchainDesc const& desc) const = 0;
        };
        class RHIApplication : public RHIObject {
        public:
            RHIApplication() = default;
            RHIApplication(RHIApplication const&) = delete;

            virtual std::vector<std::weak_ptr<RHIDevice>> EnumerateDevices() const = 0;
        };
    }
}

