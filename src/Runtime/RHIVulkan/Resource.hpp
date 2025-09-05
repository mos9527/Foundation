#pragma once
#include <RHICore/Resource.hpp>
#include "Common.hpp"

#include <vma/vk_mem_alloc.h>
#include <mutex>
namespace Foundation::RHI {
    inline VmaAllocationCreateFlags vmaAllocationFlagsFromRHIResourceHostAccess(RHIResourceHostAccess access) {
        using enum RHIResourceHostAccess;
        switch (access) {
        case ReadWrite: return VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        case WriteOnly: return VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        case Invisible:
        default:
            return {};
        }
    }
    class VulkanDevice;

    class VulkanBuffer : public RHIBuffer {
    protected:
        VulkanDevice const& m_device;
        VmaAllocation m_allocation{ nullptr };
        void* m_mapped{ nullptr };

        vk::raii::Buffer m_buffer{ nullptr };

        RHIObjectStorage<VulkanBuffer> m_aliases;

        class Arena : public RHIBuffer::Arena {
            const size_t m_size;
            VmaVirtualBlock m_block{};
            // [Allocation, {size, offset, VmaVirtualAllocation}]
            Core::FreeList<Allocation, std::tuple<size_t, size_t, VmaVirtualAllocation>> m_allocs;

            std::mutex m_mutex;
        public:
            Arena(Core::Allocator* alloc, size_t size) : m_allocs(alloc), m_size(size) {
                const VmaVirtualBlockCreateInfo info{ .size = size };
                vmaCreateVirtualBlock(&info, &m_block);
            }
            Allocation Allocate(size_t size, size_t alignment) override {
                std::scoped_lock lock(m_mutex);
                VmaVirtualAllocationCreateInfo info{ .size = size, .alignment = alignment };
                VmaVirtualAllocation alloc{};
                VkDeviceSize offset{};
                VkResult ret = vmaVirtualAllocate(m_block, &info, &alloc, &offset);
                if (ret == VK_ERROR_OUT_OF_DEVICE_MEMORY)
                    return kInvalidHandle;
                auto& [res, ainfo] = m_allocs.allocate();
                auto& [sz, off, vmaAlloc] = ainfo;
                sz = size, off = offset, vmaAlloc = alloc;
                return res;
            }
            void Free(Allocation alloc) override {
                std::scoped_lock lock(m_mutex);
                auto& [sz, off, vmaAlloc] = m_allocs.at(alloc);
                vmaVirtualFree(m_block, vmaAlloc);
                m_allocs.free(alloc);
            }
            size_t GetOffset(Allocation alloc) const override  {
                auto& [sz, off, vmaAlloc] = m_allocs.at(alloc);
                return off;
            }
            size_t GetSize(Allocation alloc) const override {
                auto& [sz, off, vmaAlloc] = m_allocs.at(alloc);
                return sz;
            }
            void Reset() override {
                std::scoped_lock lock(m_mutex);
                vmaClearVirtualBlock(m_block);
                m_allocs.clear();
            }
            ~Arena() {
                vmaClearVirtualBlock(m_block);
                vmaDestroyVirtualBlock(m_block);
            }
        } m_arena;
    public:
        // Buffer created by other means.
        const bool m_shared{ false };

        VulkanBuffer(VulkanDevice const& device, RHIBufferDesc const& desc);
        // Thin wrapper for buffers created by swapchains or other external sources (e.g. aliasing)
        VulkanBuffer(VulkanDevice const& device, RHIBufferDesc const& desc, vk::raii::Buffer&& buffer, bool shared = true);
        ~VulkanBuffer();

        inline auto& GetVkBuffer() { return m_buffer; }

        Arena& GetArena() override { return m_arena; }

        void* Map() override;
        void Flush(size_t offset, size_t size) override;
        void Unmap() override;

        RHIBufferScopedHandle<RHIBuffer> CreateAliasedBuffer(RHIBufferDesc const& desc, size_t offset) override;
        RHIBuffer* GetAliasedBuffer(Handle handle) const override;
        void DestroyAliasedBuffer(Handle handle) override;

        void DebugSetObjectName(const char* name) override;
    };

    class VulkanTextureView;
    class VulkanTexture : public RHITexture {
    protected:
        VulkanDevice const& m_device;
        VmaAllocation m_allocation{ nullptr };

        vk::raii::Image m_image{ nullptr };
        void* m_mapped{ nullptr };

        RHIObjectStorage<VulkanTexture> m_aliases;
        RHIObjectStorage<VulkanTextureView> m_views;
    public:
        // Texture created by other means, e.g. swapchain or external source
        const bool m_shared{ false };
        VulkanTexture(VulkanDevice const& device, RHITextureDesc const& desc);
        // Thin wrapper for textures created by swapchains or other external sources (e.g. aliasing)
        VulkanTexture(VulkanDevice const& device, RHITextureDesc const& desc, vk::raii::Image&& image, bool shared = true);
        ~VulkanTexture();

        inline auto& GetVkImage() const { return m_image; }

        void* Map() override;
        void Flush(size_t offset, size_t size) override;
        void Unmap() override;

        RHITextureScopedHandle<RHITextureView> CreateTextureView(RHITextureViewDesc const& desc) override;
        RHITextureView* GetImageView(Handle handle) const override;
        void DestroyImageView(Handle handle) override;

        RHITextureScopedHandle<RHITexture> CreateAliasedTexture(RHITextureDesc const& desc, size_t offset) override;
        RHITexture* GetAliasedTexture(Handle handle) const override;
        void DestroyAliasedTexture(Handle handle) override;

        void DebugSetObjectName(const char* name) override;

        inline auto const& GetDevice() const { return m_device; }
    };

    class VulkanTextureView : public RHITextureView {
    protected:
        vk::raii::ImageView m_view{ nullptr };
        VulkanTexture& m_image;
    public:
        VulkanTextureView(VulkanTexture& image, RHITextureViewDesc const& desc, vk::raii::ImageView&& view);

        inline auto const& GetVkImageView() const { return m_view; }
        inline auto const& GetImage() const { return m_image; }

        void DebugSetObjectName(const char* name) override;
    };
}
