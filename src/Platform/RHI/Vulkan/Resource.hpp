#pragma once
#include <Platform/RHI/Vulkan/Common.hpp>
#include <Platform/RHI/Resource.hpp>

#include <vma/vk_mem_alloc.h>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            class VulkanDevice;
            class VulkanResource : public virtual RHIResource {
            protected:
                VulkanDevice const& m_device;
                VmaAllocation m_allocation{ nullptr };
            public:
                VulkanResource(VulkanDevice const& device, ResourceDesc const& desc);
                inline bool IsValid() const override { return true; }
                inline const char* GetName() const override { return m_desc.name.c_str(); }
            };

            class VulkanBuffer : public VulkanResource, public RHIBuffer {
            protected:
                vk::raii::Buffer m_buffer{ nullptr };
            public:
                VulkanBuffer(VulkanDevice const& device, BufferDesc const& desc);
                ~VulkanBuffer();

                inline auto& GetVkBuffer() { return m_buffer; }

                inline bool IsValid() const override { return m_buffer != nullptr; }
                inline const char* GetName() const override { return m_desc.name.c_str(); }
            };

            class VulkanImageView;
            class VulkanImage : public RHIImage, public VulkanResource {
            protected:
                bool m_shared{ false };
                vk::raii::Image m_image{ nullptr };

                RHIObjectStorage<VulkanImageView> m_views;
            public:
                VulkanImage(VulkanDevice const& device, ImageDesc const& desc);
                VulkanImage(VulkanDevice const& device, vk::raii::Image&& image, bool shared);                
                ~VulkanImage();

                inline auto& GetVkImage() const { return m_image; }

                inline bool IsValid() const override { return m_image != nullptr; }
                inline const char* GetName() const override { return m_desc.name.c_str(); }

                RHIImageScopedHandle<RHIImageView> CreateImageView(RHIImageViewDesc const& desc) override;
                RHIImageView* GetImageView(Handle handle) const override;
                void DestroyImageView(Handle handle) override;
            };

            class VulkanImageView : public RHIImageView {
            protected:
                vk::raii::ImageView m_view{ nullptr };
                VulkanImage& m_image;
            public:
                VulkanImageView(VulkanImage& image, RHIImageViewDesc const& desc, vk::raii::ImageView&& view);

                inline bool IsValid() const override { return true; }
                inline const char* GetName() const override { return "VulkanImageView"; }

                inline auto const& GetVkImageView() const { return m_view; }
                inline auto const& GetImage() const { return m_image; }
            };
        }
    }
}
