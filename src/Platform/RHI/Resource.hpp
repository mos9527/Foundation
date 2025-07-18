#pragma once
#include <string>
#include <variant>
#include <Platform/RHI/Common.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            class RHIDevice;
            struct RHIResourceDesc {
                std::string name;
                /// Which heap the resource is allocated in
                RHIDeviceHeapType heap;
                /// How the resource can be used by the device, initially
                RHIBufferUsage usage;
                /// How the resource can be accessed by the host (CPU)
                RHIResourceHostAccess host_access;
                /// Can be shared with other devices
                bool shared;
                /// Guarantees that the host can see the latest data written by the device without explicit flush
                /// On implementations that do not support this, exceptions will be thrown when trying to create such resources.            
                bool coherent;
            };
            struct RHIBufferDesc {
                RHIResourceDesc resource;
                size_t size; // size in bytes
            };
            struct RHIImageDesc {
                RHIResourceDesc resource;
                uint32_t width;
                uint32_t height;
                RHIResourceFormat format;
                uint32_t mip_levels;
                uint32_t array_layers;
            };
            class RHIBuffer : public RHIObject {
            protected:
                const RHIDevice& m_device;
            public:
                const RHIBufferDesc m_desc;
                RHIBuffer(RHIDevice const& device, RHIBufferDesc const& desc)
                    : m_device(device), m_desc(desc) {
                }
                /// <summary>
                /// Maps the entire buffer to the host memory.
                /// Alignment is implementation-defined.
                /// Implementations MUST guarantee consecutive calls to Map() return the same pointer,
                /// therefore it's not possible to map the same resource multiple times.
                /// For caching behaviours, <see cref="RHIBufferDesc"/>
                /// </summary>               
                virtual void* Map() = 0;
                /// <summary>
                /// Flushes the mapped region to the device.
                /// Depending on the implementation, this may be a no-op.                
                /// </summary>
                virtual void Flush(size_t offset = 0, size_t size = kFullSize) = 0;
                /// <summary>
                /// Releases or unmaps a previously mapped resource.
                /// Implementations MUST guarantee that Unmap() is called at destruction time
                /// if the resource is still mapped.                
                /// </summary>
                virtual void Unmap() = 0;

                /// <summary>
                /// Creates a span that maps a contiguous region of the buffer to the host memory.
                /// Behaviour of the memory access is defined by the resource itself. e.g. RO/RW.
                /// It is undefined behaviour to access the memory without regard to the resource's host access type.
                /// For detailed mapping behaviour, <see cref="Map"/>
                /// </summary>                
                /// <param name="count">count of elements</param>                
                template<typename T> Core::StlSpan<T> MapSpan(size_t count = kFullSize) {
                    void* p = Map();
                    if (count == kFullSize)
                        count = m_desc.size / sizeof(T);
                    CHECK(count * sizeof(T) <= m_desc.size && "Buffer map range out of bounds");
                    return { static_cast<T*>(p) , count };
                }
            };
            struct RHIImageViewDesc {
                RHIResourceFormat format;
                uint32_t base_mip;
                uint32_t level_count;
                uint32_t base_array_layer;
                uint32_t layer_count;
            };
            class RHIImage;
            class RHIImageView;
            template<typename T> using RHIImageScopedHandle = RHIScopedHandle<RHIImage, T>;
            template<typename T> using RHIImageHandle = RHIHandle<RHIImage, T>;
            class RHIImage : public RHIObject {
            protected:
                const RHIDevice& m_device;
            public:
                const RHIImageDesc m_desc;
                RHIImage(RHIDevice const& device, RHIImageDesc const& desc)
                    : m_device(device), m_desc(desc) {
                }
                virtual RHIImageScopedHandle<RHIImageView> CreateImageView(RHIImageViewDesc const& desc) = 0;
                virtual RHIImageView* GetImageView(Handle handle) const = 0;
                virtual void DestroyImageView(Handle handle) = 0;
            };

            class RHIImageView : public RHIObject {
            protected:
                const RHIImage& m_image;
                const RHIImageViewDesc& m_desc;
            public:
                RHIImageView(RHIImage const& image, RHIImageViewDesc const& desc)
                    : m_image(image), m_desc(desc) {
                }
            };
            template<> struct RHIObjectTraits<RHIImageView> {
                static RHIImageView* Get(RHIImage const* image, Handle handle) {
                    return image->GetImageView(handle);
                }
                static void Destroy(RHIImage* image, Handle handle) {
                    image->DestroyImageView(handle);
                }
            };
        }
    }
}
