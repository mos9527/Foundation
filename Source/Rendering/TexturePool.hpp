#pragma once
#include <RHICore/Device.hpp>
#include <RHICore/Descriptor.hpp>
#include <Bits/Functional.hpp>
#include <Native/Filesystem.hpp>
namespace Foundation::Rendering
{
    using namespace Foundation::RHI;
    using namespace Foundation::Native;
    using namespace Foundation::Bits;
    using TexturePoolHandle = size_t;
    using TexturePair = Pair<RHIDeviceScopedObjectHandle<RHITexture>, RHITextureScopedHandle<RHITextureView>>;
    /**
     * @brief Bindless Texture Pool implementation
     *
     * See @ref Examples::TexturePoolApp for reference usage.
     */
    class TexturePool : RHIObject // pinned
    {
        const size_t m_maxTextures;
        RHIDevice* m_device;
        Allocator* m_allocator;
        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout> m_descriptorSetLayout;
        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> m_descriptorPool;
        RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet> m_descriptorSet;
        FreeList<TexturePoolHandle, Variant<TexturePair, RHITextureView*>> m_textures;

        TexturePoolHandle m_missingTextureHandle{ kInvalidHandle };
        void SetMissingTexture(uint32_t index);
        RHIDeviceIdleGuard m_idleGuard;
    public:
        TexturePool(RHIDevice* device, Allocator* allocator, uint32_t max_textures = 128);

        // docs blah blah blah
        // - allocations are usually contiguous
        // - first index is reserved for missing texture
        [[nodiscard]] TexturePoolHandle Allocate(RHITextureDesc const& desc);
        [[nodiscard]] TexturePoolHandle Allocate(RHITextureDesc const& desc, RHITextureViewDesc const& viewDesc);
        [[nodiscard]] TexturePoolHandle Allocate(RHITextureView* view);

        [[nodiscard]] RHITexture* GetTexture(TexturePoolHandle handle) const;
        [[nodiscard]] RHITextureView* GetTextureView(TexturePoolHandle handle) const;
        void Free(TexturePoolHandle handle);

        [[nodiscard]] RHIDeviceDescriptorSet* GetDescriptorSet() const { return m_descriptorSet.Get(); }
        [[nodiscard]] RHIDeviceDescriptorSetLayout* GetDescriptorSetLayout() const { return m_descriptorSetLayout.Get(); }
    };
}