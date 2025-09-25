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
        VulkanDevice const& mDevice;
        VmaAllocation mAllocation{ nullptr };
        void* mMapped{ nullptr };

        vk::raii::Buffer mBuffer{ nullptr };

        RHIObjectPool<VulkanBuffer> mAliases;
    public:
        // Buffer created by other means.
        const bool mShared{ false };

        VulkanBuffer(VulkanDevice const& device, RHIBufferDesc const& desc);
        // Thin wrapper for buffers created by swapchains or other external sources (e.g. aliasing)
        VulkanBuffer(VulkanDevice const& device, RHIBufferDesc const& desc, vk::raii::Buffer&& buffer, bool shared = true);
        ~VulkanBuffer() override;

        auto& GetVkBuffer() { return mBuffer; }

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
        VulkanDevice const& mDevice;
        VmaAllocation mAllocation{ nullptr };

        vk::raii::Image mImage{ nullptr };
        void* mMapped{ nullptr };

        RHIObjectPool<VulkanTexture> mAliases;
        RHIObjectPool<VulkanTextureView> mViews;
    public:
        // Texture created by other means, e.g. swapchain or external source
        const bool mShared{ false };
        VulkanTexture(VulkanDevice const& device, RHITextureDesc const& desc);
        // Thin wrapper for textures created by swapchains or other external sources (e.g. aliasing)
        VulkanTexture(VulkanDevice const& device, RHITextureDesc const& desc, vk::raii::Image&& image, bool shared = true);
        ~VulkanTexture() override;

        auto& GetVkImage() const { return mImage; }

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

        auto const& GetDevice() const { return mDevice; }
    };

    class VulkanTextureView : public RHITextureView {
    protected:
        vk::raii::ImageView mView{ nullptr };
        VulkanTexture& mImage;
    public:
        VulkanTextureView(VulkanTexture& image, RHITextureViewDesc const& desc, vk::raii::ImageView&& view);

        [[nodiscard]] RHITexture* GetTexture() const override { return &mImage; }

        [[nodiscard]] auto const& GetVkImageView() const { return mView; }
        [[nodiscard]] auto const& GetImage() const { return mImage; }

        void DebugSetObjectName(const char* name) override;
    };
}
