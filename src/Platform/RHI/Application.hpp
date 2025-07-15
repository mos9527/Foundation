#pragma once
#include <span>
#include <Platform/Application.hpp>
#include <Platform/RHI/Device.hpp>

namespace Foundation {
    namespace Platform {
        namespace RHI {
            class RHIDevice;
            class RHIApplication : public RHIObject {
            public:
                RHIApplication() = default;
                RHIApplication(RHIApplication const&) = delete;

                virtual std::span<const RHIDevice::DeviceDesc> EnumerateDevices() const = 0;

                /// <summary>
                /// Create and instantiate a RHIDevice.                
                /// </summary>                
                /// <param name="window">When nullptr, the created device MAY be unable to present.</param>               
                virtual RHIApplicationScopedObjectHandle<RHIDevice> CreateDevice(RHIDevice::DeviceDesc const& desc, Window* window = nullptr) = 0;
                virtual RHIDevice* GetDevice(Handle handle) const = 0;
                virtual void DestroyDevice(Handle handle) = 0;
            };
            template<> struct RHIObjectTraits<RHIDevice> {
                static RHIDevice* Get(RHIApplication const* app, Handle handle) {
                    return app->GetDevice(handle);
                }
                static void Destroy(RHIApplication* app, Handle handle) {
                    app->DestroyDevice(handle);
                }
            };
        }
    }
}
