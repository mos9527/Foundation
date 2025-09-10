#pragma once
#include "Common.hpp"
namespace Foundation::RHI {
    class RHIDevice;
    struct RHIResourceDesc {
        bool is_alias{ false }; // If true, the resource is an alias of another resource.
        /// Which heap the resource is allocated in
        RHIDeviceHeapType heap{ RHIDeviceHeapType::Local };
        /// How the resource can be accessed by the host (CPU)
        RHIResourceHostAccess host_access{ RHIResourceHostAccess::Invisible };
        /// Can be shared with other devices
        bool shared{ false };
        /// Guarantees that the host can see the latest data written by the device without explicit flush
        /// On implementations that do not support this, exceptions will be thrown when trying to create such resources.            
        bool coherent{ false };
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
        const RHIDevice& m_device;
    public:
        const RHIBufferDesc m_desc;
        RHIBuffer(RHIDevice const& device, RHIBufferDesc const& desc)
            : m_device(device), m_desc(desc) {
        }
        /**
         * @brief Proxy object to provide generic sub-buffer allocation within a buffer.                
         *
         * Implementations should guarantee thread-safety of the arena itself.          
         */
        struct Arena : public RHIObject {
            using Allocation = size_t;
            /**
             * @brief Allocates a sub-region of the buffer with the given size and alignment.            
             */
            /// <returns>An opaque handle representing the allocated sub-region. kInvalidHandle if allocation fails.</returns>            
            virtual Allocation Allocate(size_t size, size_t alignment) = 0;
            /**
             * @brief Frees a previously allocated sub-region of the buffer.
             */
            virtual void Free(Allocation allocation) = 0;
            /**
             * @brief Retrieves the offset of a previously allocated sub-region of the buffer.
             * This is not a raw pointer - one may expect to use this in CPU copy commands
             * or GPU shader code, or an offest in mapped memory if the resource is CPU visible.
             */
            virtual size_t GetOffset(Allocation alloc) const = 0;
            /**
             * @brief Retrieves the size of a previously allocated sub-region of the buffer.
             */
            virtual size_t GetSize(Allocation alloc) const = 0;
            /**
             * @brief Resets the arena, freeing all CPU-tracked allocations.
             */
            virtual void Reset() = 0;
        };
        /**
         * @brief Sub-buffer allocation arena capable of generic alloc/free
         * operations.        
         */
        virtual Arena& GetArena() = 0;
        /**
         * @brief Maps the entire buffer to the host memory.
         * Alignment is implementation-defined.
         * Implementations MUST guarantee consecutive calls to Map() return the same pointer,
         * therefore it's not possible to map the same resource multiple times.
         * For caching behaviours, <see cref="RHIBufferDesc"/>
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
        template<typename T> Core::Span<T> MapSpan(size_t count = kFullSize) {
            void* p = Map();
            if (count == kFullSize)
                count = m_desc.size / sizeof(T);
            CHECK(count * sizeof(T) <= m_desc.size && "Buffer map range out of bounds");
            return { static_cast<T*>(p) , count };
        }

        [[nodiscard]] virtual RHIBufferScopedHandle<RHIBuffer> CreateAliasedBuffer(RHIBufferDesc const& desc, size_t offset = 0) = 0;
        virtual RHIBuffer* GetAliasedBuffer(Handle handle) const = 0;
        virtual void DestroyAliasedBuffer(Handle handle) = 0;

        virtual void DebugSetObjectName(const char* name) = 0;
    };
    struct RHITextureDesc {
        RHIResourceDesc resource{};
        RHITextureDimension dimension{ RHITextureDimension::e2D };
        RHITextureUsage usage{};
        RHIExtent3D extent{ 1, 1, 1 }; // Width, height, depth of the image.
        RHIResourceFormat format{ RHIResourceFormat::Undefined };
        RHIMultisampleCount sample_count{ RHIMultisampleCount::e1 }; // For MSAA
        uint32_t mip_levels{ 1 };
        uint32_t array_layers{ 1 }; // No. of images in an image array.
        RHITextureLayout initial_layout{ RHITextureLayout::Undefined };        
    };
    class RHITexture;
    class RHITextureView;
    template<typename T> using RHITextureScopedHandle = RHIScopedHandle<RHITexture, T>;
    template<typename T> using RHITextureHandle = RHIHandle<RHITexture, T>;
    struct RHITextureSubresourceLayer {
        RHITextureAspectFlag access{ RHITextureAccessFlagBits::Color };
        uint32_t mip_level{ 0 };
        uint32_t base_array_layer{ 0 };
        uint32_t layer_count{ 1 };
    };
    struct RHITextureSubresourceRange {
        RHITextureSubresourceLayer layer;
        uint32_t mip_count{ 1 }; // Number of mip levels in the range

        inline Pair<uint32_t, uint32_t> GetMipLevelRange() const {
            return { layer.mip_level, layer.mip_level + mip_count - 1 };
        }
        inline Pair<uint32_t, uint32_t> GetArrayLayerRange() const {
            return { layer.base_array_layer, layer.base_array_layer + layer.layer_count - 1 };
        }
    };
    struct RHITextureViewDesc {
        RHIResourceFormat format;
        RHITextureDimension dimension{ RHITextureDimension::e2D };
        RHITextureSubresourceRange range{};
    };
    class RHITexture : public RHIObject {
    protected:
        const RHIDevice& m_device;
    public:
        const RHITextureDesc m_desc;
        RHITexture(RHIDevice const& device, RHITextureDesc const& desc)
            : m_device(device), m_desc(desc) {
        }

        virtual void* Map() = 0;
        virtual void Flush(size_t offset, size_t size) = 0;
        virtual void Unmap() = 0;

        [[nodiscard]] virtual RHITextureScopedHandle<RHITextureView> CreateTextureView(RHITextureViewDesc const& desc) = 0;
        virtual RHITextureView* GetImageView(Handle handle) const = 0;
        virtual void DestroyImageView(Handle handle) = 0;

        [[nodiscard]] virtual RHITextureScopedHandle<RHITexture> CreateAliasedTexture(RHITextureDesc const& desc, size_t offset = 0) = 0;
        virtual RHITexture* GetAliasedTexture(Handle handle) const = 0;
        virtual void DestroyAliasedTexture(Handle handle) = 0;

        virtual void DebugSetObjectName(const char* name) = 0;
    };
    class RHITextureView : public RHIObject {
    protected:
        const RHITexture& m_image;
        const RHITextureViewDesc& m_desc;
    public:
        RHITextureView(RHITexture const& image, RHITextureViewDesc const& desc)
            : m_image(image), m_desc(desc) {
        }

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
