#pragma once
#include <Platform/Application.hpp>
#include <Platform/RHI/Details/Details.hpp>
#include <Platform/RHI/Device.hpp>

namespace Foundation {
    namespace Platform {
        namespace RHI {
            class RHIDevice;
            class RHIApplication : public RHIObject {
            public:
                RHIApplication() = default;
                RHIApplication(RHIApplication const&) = delete;

                virtual std::vector<RHIDevice::DeviceDesc> EnumerateDevices() const = 0;

                virtual RHIApplicationScopedObjectHandle<RHIDevice> CreateDevice(RHIDevice::DeviceDesc const& desc) = 0;
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
