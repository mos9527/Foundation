#pragma once
#include "Device.hpp"
namespace Foundation::RHI {
    class RHIDevice;
    class RHIApplication;
    struct RHIFileInfo
    {
        uint64_t size;
        // Seconds since UNIX epoch
        long long ctime, mtime, atime;
    };
    /**
     * @brief The root object of everything RHI.
     * Implementation of this class inherently defines the RHI backend.
     */
    class RHIApplication : public RHIObject {
    public:
        RHIApplication() = default;
        RHIApplication(RHIApplication const&) = delete;
        // RHI
        [[nodiscard]] virtual Span<const RHIDevice::DeviceDesc> EnumerateDevices() const = 0;         
        [[nodiscard]] virtual RHIApplicationScopedHandle<RHIDevice> CreateDevice(RHIDevice::DeviceDesc const& desc) = 0;
        [[nodiscard]] virtual RHIDevice* GetDevice(Handle handle) const = 0;
        virtual void DestroyDevice(Handle handle) = 0;

        // Filesystem
        /**
         * @brief Retrives path relative to the application executable
         */
        [[nodiscard]] virtual String ResolveRelativePathBase(StringView path = "") const = 0;
        /**
         * @brief Retrives path that's writable and persistent
         */
        [[nodiscard]] virtual String ResolveRelativePathData(StringView path = "") const = 0;
        [[nodiscard]] virtual Optional<RHIFileInfo> QueryFileInfo(StringView path) const = 0;
        virtual bool CreateDirectory(StringView path) const = 0;
        virtual bool RemoveDirectory(StringView path) const = 0;
        virtual bool RemoveFile(StringView path) const = 0;
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
