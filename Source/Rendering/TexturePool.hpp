#pragma once
#include <RHICore/Device.hpp>
#include <RHICore/Descriptor.hpp>
#include <Bits/Functional.hpp>
#include <Native/Filesystem.hpp>
namespace Foundation::Rendering
{
    using namespace RHI;
    using namespace Bits;
    using TexturePoolHandle = size_t;
    using TexturePair = Pair<RHIDeviceScopedObjectHandle<RHITexture>, RHITextureScopedHandle<RHITextureView>>;
    /**
     * @brief Bindless Texture Pool implementation
     *
     * See @ref Examples::TexturePoolApp for reference usage.
     */
    class TexturePool : RHIObject // pinned
    {
        const size_t mMaxTextures;
        RHIDevice* mDevice;
        Allocator* mAllocator;
        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout> mDescriptorSetLayout;
        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> mDescriptorPool;
        RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet> mDescriptorSet;
        Pool<TexturePoolHandle, Variant<TexturePair, RHITextureView*>> mTextures;

        TexturePoolHandle mMissingTextureHandle{ kInvalidHandle };
        void SetMissingTexture(uint32_t index);
        RHIDeviceIdleGuard mIdleGuard;
    public:
        TexturePool(RHIDevice* device, Allocator* allocator, uint32_t max_textures = 128);

        [[nodiscard]] TexturePoolHandle Allocate(RHITextureDesc const& desc);
        [[nodiscard]] TexturePoolHandle Allocate(RHITextureDesc const& desc, RHITextureViewDesc const& viewDesc);
        [[nodiscard]] TexturePoolHandle Allocate(RHITextureView* view);

        [[nodiscard]] RHITexture* GetTexture(TexturePoolHandle handle) const;
        [[nodiscard]] RHITextureView* GetTextureView(TexturePoolHandle handle) const;
        void Free(TexturePoolHandle handle);

        [[nodiscard]] RHIDeviceDescriptorSet* GetDescriptorSet() const { return mDescriptorSet.Get(); }
        [[nodiscard]] RHIDeviceDescriptorSetLayout* GetDescriptorSetLayout() const { return mDescriptorSetLayout.Get(); }
    };
}