#pragma once
#include "Device.hpp"

namespace Foundation::RHI {
    class RHIDevice;
    class RHIApplication;
    /**
     * @brief Opaque window object of respective RHI backends.
     */
    class RHIWindow : public RHIObject
    {
    protected:
        const RHIApplication& mApp;
    public:
        struct WindowDesc
        {
            StringView title;
            int width, height;
            int platformFlags;
        };
        RHIWindow(RHIApplication const& app) : mApp(app) {}
    };
    /**
     * @brief The root object of everything RHI.
     * Implementation of this class inherently defines the RHI backend.
     */
    class RHIApplication : public RHIObject {
    public:
        RHIApplication() = default;
        RHIApplication(RHIApplication const&) = delete;

        [[nodiscard]] virtual Span<const RHIDevice::DeviceDesc> EnumerateDevices() const = 0;


        [[nodiscard]] virtual RHIApplicationScopedObjectHandle<RHIWindow> CreateWindow(RHIWindow::WindowDesc const& desc) = 0;
        [[nodiscard]] virtual RHIWindow* GetWindow(Handle handle) const = 0;
        virtual void DestroyWindow(Handle handle) = 0;

        [[nodiscard]] virtual RHIApplicationScopedObjectHandle<RHIDevice> CreateDevice(RHIDevice::DeviceDesc const& desc, RHIWindow* window) = 0;
        [[nodiscard]] virtual RHIDevice* GetDevice(Handle handle) const = 0;
        virtual void DestroyDevice(Handle handle) = 0;
    };
    template<> struct RHIObjectTraits<RHIApplication, RHIWindow>
    {
        static RHIWindow const* Get(RHIApplication const* app, Handle handle) {
            return app->GetWindow(handle);
        }
        static void Destroy(RHIApplication* app, Handle handle) {
            app->DestroyWindow(handle);
        }
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
