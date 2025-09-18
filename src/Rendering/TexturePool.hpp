#pragma once
#include <RenderCore/RHICore/Device.hpp>
#include <RenderCore/RHICore/Descriptor.hpp>
#include <Native/Filesystem.hpp>
namespace Foundation::Rendering
{
    using namespace Foundation::RHI;
    using namespace Foundation::Native;
    using TextureHandle = size_t;
    using TexturePair = Pair<RHIDeviceScopedObjectHandle<RHITexture>, RHITextureScopedHandle<RHITextureView>>;
    class TexturePool : RHIObject // pinned
    {
        const size_t m_maxTextures;
        RHIDevice* m_device;
        Allocator* m_allocator;
        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout> m_descriptorSetLayout;
        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> m_descriptorPool;
        RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet> m_descriptorSet;
        FreeList<TextureHandle, TexturePair> m_textures;

        TextureHandle m_missingTextureHandle{ kInvalidHandle };
        void SetMissingTexture(uint32_t index);
    public:
        TexturePool(RHIDevice* device, Allocator* allocator, uint32_t max_textures = 128);

        TextureHandle Allocate(RHITextureDesc const& desc);
        TextureHandle Allocate(RHITextureDesc const& desc, RHITextureViewDesc const& viewDesc);
        RHITexture* GetTexture(TextureHandle handle) const { return m_textures.at(handle).first.Get(); }
        RHITextureView* GetTextureView(TextureHandle handle) { return m_textures.at(handle).second.Get(); }
        void Free(TextureHandle handle);

        RHIDeviceDescriptorSet* GetDescriptorSet() const { return m_descriptorSet.Get(); }
    };
}