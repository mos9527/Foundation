#pragma once
#include <Native/Application.hpp>
#include "Device.hpp"

namespace Foundation::RHI {
    class RHIDevice;
    /**
     * @brief The root object of everything RHI.
     * Implementation of this class inherently defines the RHI backend.
     */
    class RHIApplication : public RHIObject {
    public:
        RHIApplication() = default;
        RHIApplication(RHIApplication const&) = delete;

        virtual Core::StlSpan<const RHIDevice::DeviceDesc> EnumerateDevices() const = 0;
           
        virtual RHIApplicationScopedObjectHandle<RHIDevice> CreateDevice(RHIDevice::DeviceDesc const& desc, Native::NativeWindow* window = nullptr) = 0;
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
