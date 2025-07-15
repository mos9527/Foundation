#pragma once
#include <string>
#include <Platform/RHI/Common.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            class RHIDevice;
            class RHIResource : public RHIObject {
            protected:
                const RHIDevice& m_device;
            public:
                struct ResourceDesc {
                    std::string name;
                    RHIDeviceHeapType heap;
                    RHIBufferUsage usage;
                    bool shared;
                } const m_desc;

                RHIResource(RHIDevice const& device, ResourceDesc const& desc) 
                    : m_device(device), m_desc(desc) {}
            };

            // !! TODO buffer views
            class RHIBuffer : public virtual RHIResource {
            public:
                struct BufferDesc : public ResourceDesc {
                    size_t size;
                } const m_desc;
                RHIBuffer(RHIDevice const& device, BufferDesc const& desc)
                    : RHIResource(device, desc), m_desc(desc) {}
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
            class RHIImage : public virtual RHIResource {
            public:
                struct ImageDesc : public ResourceDesc {
                    uint32_t width;
                    uint32_t height;
                    RHIResourceFormat format;
                    uint32_t mip_levels;
                    uint32_t array_layers;
                } const m_desc;
                RHIImage(RHIDevice const& device, ImageDesc const& desc)
                    : RHIResource(device, desc), m_desc(desc) {}
                
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
                    : m_image(image), m_desc(desc) {}
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
