#pragma once
#include "Common.hpp"
#include "Core/Logging.hpp"
namespace Foundation::RHI {
    class RHIDevice;
    struct RHIResourceDesc {
        bool isAlias{ false }; // If true, the resource is an alias of another resource.
        /// Which heap the resource is allocated in
        RHIDeviceHeapType heap{ RHIDeviceHeapType::Local };
        /// How the resource can be accessed by the host (CPU)
        RHIResourceHostAccess hostAccess{ RHIResourceHostAccess::Invisible };
        /// Can be shared with other device queues
        bool shared{ false };
        /// With shared=true, the types of queues that are allowed access
        /// indexed by @ref RHIDeviceQueueType values
        RHIDeviceQueueFlags sharedQueues{};
        /// Guarantees that the host can see the latest data written by the device without explicit flush
        /// On implementations that do not support this, exceptions will be thrown when trying to create such resources.            
        bool coherent{ false };
        /// Hint that the resource will be used for staging
        /// With this flag - the resource may be not host-visible regardless of the host_access flag.
        /// This is a performance hint, and may be ignored by implementations.
        bool staging{ false };
    };
    struct RHIBufferDesc {
        RHIResourceDesc resource{};
        /// How the buffer can be used by the device, initially
        RHIBufferUsage usage{};
        size_t size{}; // size in bytes
    };
    class RHIBuffer;
    template<typename T> using RHIBufferScopedHandle = RHIScopedHandle<RHIBuffer, T>;
    template<typename T> using RHIBufferHandle = RHIHandle<RHIBuffer, T>;
    class RHIBuffer : public RHIObject {
    protected:
        const RHIDevice& mDevice;
    public:
        const RHIBufferDesc mDesc;
        RHIBuffer(RHIDevice const& device, RHIBufferDesc const& desc)
            : mDevice(device), mDesc(desc) {
        }
        /**
         * @brief Maps the entire buffer to the host memory.
         * Alignment is implementation-defined.
         * Implementations MUST guarantee consecutive calls to Map() return the same pointer,
         * therefore it's not possible to map the same resource multiple times.
         * For caching behaviours, @ref=RHIBufferDesc
         */
        virtual void* Map() = 0;
        /**
         * @brief Flushes the mapped region to the device.
         * Depending on the implementation, this may be a no-op.                
         */
        virtual void Flush(size_t offset = 0, size_t size = kFullSize) = 0;
        /**
         * @brief Releases or unmaps a previously mapped resource.
         * Implementations MUST guarantee that Unmap() is called at destruction time
         * if the resource is still mapped.                
         */
        virtual void Unmap() = 0;

        /**
         * @brief Creates a span that maps a contiguous region of the buffer to the host memory.
         * Behaviour of the memory access is defined by the resource itself. e.g. RO/RW.
         * It is undefined behaviour to access the memory without regard to the resource's host access type.
         * For detailed mapping behaviour, <see cref="Map"/>
         */
        /// <param name="count">count of elements</param>                
        template<typename T> Span<T> MapSpan(size_t count = kFullSize) {
            void* p = Map();
            if (count == kFullSize)
                count = mDesc.size / sizeof(T);
            CHECK_MSG(count * sizeof(T) <= mDesc.size, "Buffer map range out of bounds");
            return { static_cast<T*>(p) , count };
        }

        [[nodiscard]] virtual RHIBufferScopedHandle<RHIBuffer> CreateAliasedBuffer(RHIBufferDesc const& desc, size_t offset = 0) = 0;
        [[nodiscard]] virtual RHIBuffer* GetAliasedBuffer(Handle handle) const = 0;
        virtual void DestroyAliasedBuffer(Handle handle) = 0;

        virtual void DebugSetObjectName(const char* name) = 0;
    };
    struct RHITextureDesc {
        RHIResourceDesc resource{};
        RHITextureDimension dimension{ RHITextureDimension::E2D };
        RHITextureUsage usage{};
        RHIExtent3D extent{ 1, 1, 1 }; // Width, height, depth of the image.
        RHIResourceFormat format{ RHIResourceFormat::Undefined };
        RHIMultisampleCount sampleCount{ RHIMultisampleCount::E1 }; // For MSAA
        uint32_t mipLevels{ 1 };
        uint32_t arrayLayers{ 1 }; // No. of images in an image array.
        RHITextureLayout initialLayout{ RHITextureLayout::Undefined };        
    };
    class RHITexture;
    class RHITextureView;
    template<typename T> using RHITextureScopedHandle = RHIScopedHandle<RHITexture, T>;
    template<typename T> using RHITextureHandle = RHIHandle<RHITexture, T>;
    struct RHITextureSubresourceLayer {
        RHITextureAspectFlag aspect{};
        uint32_t mipLevel{ 0 };
        uint32_t baseArrayLayer{ 0 };
        uint32_t layerCount{ 1 };
    };
    struct RHITextureSubresourceRange {
        // Single mip level and array layer range
        RHITextureSubresourceLayer layer;
        // Number of mip levels in the range
        uint32_t mipCount;
        /* @brief Mip level used (inclusive) */
        [[nodiscard]] inline Pair<uint32_t, uint32_t> GetMipLevelRange() const {
            return { layer.mipLevel, layer.mipLevel + mipCount - 1 };
        }
        /* @brief Array layers used (inclusive) */
        [[nodiscard]] inline Pair<uint32_t, uint32_t> GetArrayLayerRange() const {
            return { layer.baseArrayLayer, layer.baseArrayLayer + layer.layerCount - 1 };
        }
        /**
         * @brief Helper function to create a Subresource Range with default parameters
         * @note The created range is validated, and will throw if invalid.
         * @param aspect Defaults to @ref RHITextureAspectFlagBits::Color
         * @param base_mip_level Defaults to 0
         * @param mip_count Defaults to 1
         * @param base_array_layer Defaults to 0
         * @param layer_count Defaults to 1
         */
        static RHITextureSubresourceRange Create(RHITextureAspectFlag aspect = RHITextureAspectFlagBits::Color, uint32_t base_mip_level = 0, uint32_t mip_count = 1, uint32_t base_array_layer = 0, uint32_t layer_count = 1)
        {
            RHITextureSubresourceRange res{
                .layer = {
                    .aspect = aspect,
                    .mipLevel = base_mip_level,
                    .baseArrayLayer = base_array_layer,
                    .layerCount = layer_count,
                },
                .mipCount = mip_count,
            };
            CHECK_MSG(res.IsValid(), "Invalid Subresource Range is being created!");
            return res;
        }
        [[nodiscard]] constexpr bool IsValid() const
        {
            return layer.aspect.value && mipCount && layer.layerCount;
        }
    };
    struct RHITextureViewDesc {
        RHIResourceFormat format;
        RHITextureDimension dimension{ RHITextureDimension::E2D };
        RHITextureSubresourceRange range{};
    };
    class RHITexture : public RHIObject {
    protected:
        const RHIDevice& mDevice;
    public:
        const RHITextureDesc mDesc;
        RHITexture(RHIDevice const& device, RHITextureDesc const& desc)
            : mDevice(device), mDesc(desc) {
        }

        virtual void* Map() = 0;
        virtual void Flush(size_t offset, size_t size) = 0;
        virtual void Unmap() = 0;

        [[nodiscard]] virtual RHITextureScopedHandle<RHITextureView> CreateTextureView(RHITextureViewDesc const& desc) = 0;
        [[nodiscard]] virtual RHITextureView* GetImageView(Handle handle) const = 0;
        virtual void DestroyImageView(Handle handle) = 0;

        [[nodiscard]] virtual RHITextureScopedHandle<RHITexture> CreateAliasedTexture(RHITextureDesc const& desc, size_t offset = 0) = 0;
        [[nodiscard]] virtual RHITexture* GetAliasedTexture(Handle handle) const = 0;
        virtual void DestroyAliasedTexture(Handle handle) = 0;

        virtual void DebugSetObjectName(const char* name) = 0;
    };
    class RHITextureView : public RHIObject {
    protected:
        const RHITexture& mImage;
        const RHITextureViewDesc& mDesc;
    public:
        RHITextureView(RHITexture const& image, RHITextureViewDesc const& desc)
            : mImage(image), mDesc(desc) {
        }
        [[nodiscard]] virtual RHITexture* GetTexture() const = 0;
        virtual void DebugSetObjectName(const char* name) = 0;
    };
    template<> struct RHIObjectTraits<RHITexture, RHITextureView> {
        static RHITextureView* Get(RHITexture const* image, Handle handle) {
            return image->GetImageView(handle);
        }
        static void Destroy(RHITexture* image, Handle handle) {
            image->DestroyImageView(handle);
        }
    };
    template<> struct RHIObjectTraits<RHIBuffer, RHIBuffer> {
        static RHIBuffer* Get(RHIBuffer const* buffer, Handle handle) {
            return buffer->GetAliasedBuffer(handle);
        }
        static void Destroy(RHIBuffer* buffer, Handle handle) {
            buffer->DestroyAliasedBuffer(handle);
        }
    };
    template<> struct RHIObjectTraits<RHITexture, RHITexture> {
        static RHITexture* Get(RHITexture const* texture, Handle handle) {
            return texture->GetAliasedTexture(handle);
        }
        static void Destroy(RHITexture* texture, Handle handle) {
            texture->DestroyAliasedTexture(handle);
        }
    };
}
