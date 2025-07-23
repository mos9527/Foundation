#pragma once
#include <Core/Platform/Application.hpp>
#include "Device.hpp"
namespace Foundation::RHI {
    class RHIDevice;
    class RHIApplication : public RHIObject {
    public:
        RHIApplication() = default;
        RHIApplication(RHIApplication const&) = delete;

        virtual Core::StlSpan<const RHIDevice::DeviceDesc> EnumerateDevices() const = 0;

        /// <summary>
        /// Create and instantiate a RHIDevice.                
        /// </summary>                
        /// <param name="window">When nullptr, the created device MAY be unable to present.</param>               
        virtual RHIApplicationScopedObjectHandle<RHIDevice> CreateDevice(RHIDevice::DeviceDesc const& desc, Core::Window* window = nullptr) = 0;
        virtual RHIDevice* GetDevice(Handle handle) const = 0;
        virtual void DestroyDevice(Handle handle) = 0;
    };
    template<> struct RHIObjectTraits<RHIApplication, RHIDevice> {
        static RHIDevice* Get(RHIApplication const* app, Handle handle) {
            return app->GetDevice(handle);
        }
        static void Destroy(RHIApplication* app, Handle handle) {
            app->DestroyDevice(handle);
        }
    };
}
