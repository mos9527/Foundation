#pragma once
#include <Platform/RHI/Vulkan/Common.hpp>
#include <Platform/RHI/Resource.hpp>

#include <vma/vk_mem_alloc.h>
namespace Foundation {
    namespace Platform {
        namespace RHI {
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
            public:
                VulkanBuffer(VulkanDevice const& device, RHIBufferDesc const& desc);
                ~VulkanBuffer();

                inline auto& GetVkBuffer() { return m_buffer; }

                inline bool IsValid() const override { return m_buffer != nullptr; }
                inline const char* GetName() const override { return m_desc.resource.name.c_str(); }

                void* Map() override;
                void Flush(size_t offset, size_t size) override;
                void Unmap() override;
            };

            class VulkanImageView;
            class VulkanImage : public RHIImage {
            protected:
                VulkanDevice const& m_device;
                VmaAllocation m_allocation{ nullptr };
                RHIObjectStorage<VulkanImageView> m_views;

                vk::raii::Image m_image{ nullptr };
            public:
                const bool m_shared{ false };
                VulkanImage(VulkanDevice const& device, RHIImageDesc const& desc);
                VulkanImage(VulkanDevice const& device, vk::raii::Image&& image, bool shared);                
                ~VulkanImage();

                inline auto& GetVkImage() const { return m_image; }

                inline bool IsValid() const override { return m_image != nullptr; }
                inline const char* GetName() const override { return m_desc.resource.name.c_str(); }

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
