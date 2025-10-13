#pragma once
#include <Bits/Functional.hpp>
#include <Native/Filesystem.hpp>
#include <RHICore/Descriptor.hpp>
#include <RHICore/Device.hpp>
#include <Async/Future.hpp>
namespace Foundation::Rendering
{
    using namespace RHI;
    using namespace Bits;
    using TexturePoolHandle = uint32_t;
    using TexturePair = Pair<RHIDeviceScopedObjectHandle<RHITexture>, RHITextureScopedHandle<RHITextureView>>;
    constexpr TexturePoolHandle kInvalidTexturePoolHandle = ~0u;
    /**
     * @brief Bindless Texture Pool implementation
     *
     * See @ref Examples::TexturePoolApp for reference usage.
     */
    class TexturePool : RHIObject // pinned
    {
        const uint32_t mMaxTextures;
        RHIDevice* mDevice;
        Allocator* mAllocator;
        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout> mDescriptorSetLayout;
        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> mDescriptorPool;
        RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet> mDescriptorSet;
        Pool<TexturePoolHandle, Variant<TexturePair, RHITextureView*>> mTextures;

        TexturePoolHandle mMissingTextureHandle{ kInvalidTexturePoolHandle };
        void SetMissingTexture(uint32_t index);
        RHIDeviceIdleGuard mIdleGuard;

        Async::Mutex mMutex;
    public:
        TexturePool(RHIDevice* device, Allocator* allocator, uint32_t max_textures = 128);

        [[nodiscard]] TexturePoolHandle Allocate(RHITextureDesc const& desc);
        [[nodiscard]] TexturePoolHandle Allocate(RHITextureDesc const& desc, RHITextureViewDesc const& viewDesc);
        [[nodiscard]] TexturePoolHandle Allocate(RHITextureView* view);

        [[nodiscard]] RHITexture* GetTexture(TexturePoolHandle handle) const;
        [[nodiscard]] RHITextureView* GetTextureView(TexturePoolHandle handle) const;
        void Free(TexturePoolHandle handle);

        bool Contains(TexturePoolHandle handle) const { return mTextures.Contains(handle); }

        [[nodiscard]] RHIDeviceDescriptorSet* GetDescriptorSet() const { return mDescriptorSet.Get(); }
        [[nodiscard]] RHIDeviceDescriptorSetLayout* GetDescriptorSetLayout() const
        {
            return mDescriptorSetLayout.Get();
        }
    };
} // namespace Foundation::Rendering
