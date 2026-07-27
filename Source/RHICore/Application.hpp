#pragma once
#include "Device.hpp"
namespace Foundation::RHI {
    class RHIDevice;
    class RHIApplication;
    struct RHIFileInfo
    {
        // In bytes
        uint64_t size;
        // Seconds since UNIX epoch
        long long ctime, mtime, atime;
        // Is this a directory?
        bool isDirectory;
    };
    /**
     * @brief Directory iterator callback.
     * @return true to continue iterating, false to stop.
     */
    using RHIDirectoryIteratorCallback = bool (*)(void* userData, StringView directory, StringView file);
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
        /**
         * @brief Iterate over the files in a directory, calling the provided callback for each file.
         * @param cb Callback invoked per entry; returns true to continue iterating, false to stop.
         * @return false if the directory could not be enumerated.
         */
        virtual bool IterateDirectory(StringView path, RHIDirectoryIteratorCallback cb, void* userData) const = 0;
        virtual bool CreateDirectory(StringView path) const = 0;
        virtual bool RemoveDirectory(StringView path) const = 0;
        virtual bool RemoveFile(StringView path) const = 0;
        /**
         * @brief Iterate over the files in a directory, calling the provided callback for each file.
         * @tparam T Lambda taking (StringView directory, StringView file) -> bool
         *           Returns true to continue iterating, false to stop.
         */
        template<typename T>
        bool IterateDirectory(StringView path, T&& cb) const
        {
            auto wrapper = [](void* userData, StringView directory, StringView file) -> bool
            {
                auto& func = *static_cast<std::remove_reference_t<T>*>(userData);
                return func(directory, file);
            };
            return IterateDirectory(path, wrapper, &cb);
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
