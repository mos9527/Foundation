#pragma once
#include <RHICore/Resource.hpp>
#include <vk_mem_alloc.h>

#include "Common.hpp"
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

        RHIObjectPool<VulkanBuffer> m_aliases;
    public:
        // Buffer created by other means.
        const bool m_shared{ false };

        VulkanBuffer(VulkanDevice const& device, RHIBufferDesc const& desc);
        // Thin wrapper for buffers created by swapchains or other external sources (e.g. aliasing)
        VulkanBuffer(VulkanDevice const& device, RHIBufferDesc const& desc, vk::raii::Buffer&& buffer, bool shared = true);
        ~VulkanBuffer() override;

        auto& GetVkBuffer() { return m_buffer; }

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

        RHIObjectPool<VulkanTexture> m_aliases;
        RHIObjectPool<VulkanTextureView> m_views;
    public:
        // Texture created by other means, e.g. swapchain or external source
        const bool m_shared{ false };
        VulkanTexture(VulkanDevice const& device, RHITextureDesc const& desc);
        // Thin wrapper for textures created by swapchains or other external sources (e.g. aliasing)
        VulkanTexture(VulkanDevice const& device, RHITextureDesc const& desc, vk::raii::Image&& image, bool shared = true);
        ~VulkanTexture() override;

        auto& GetVkImage() const { return m_image; }

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

        auto const& GetDevice() const { return m_device; }
    };

    class VulkanTextureView : public RHITextureView {
    protected:
        vk::raii::ImageView m_view{ nullptr };
        VulkanTexture& m_image;
    public:
        VulkanTextureView(VulkanTexture& image, RHITextureViewDesc const& desc, vk::raii::ImageView&& view);

        RHITexture* GetTexture() const override { return &m_image; }

        auto const& GetVkImageView() const { return m_view; }
        auto const& GetImage() const { return m_image; }

        void DebugSetObjectName(const char* name) override;
    };
}
