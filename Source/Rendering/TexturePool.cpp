#include "TexturePool.hpp"
#include <Rendering/UploadContext.hpp>
using namespace Foundation::Rendering;
const uint32_t kInvalidTexture[4] = {0xFFFF00FF, 0x00000000, 0x00000000, 0xFFFF00FF}; // RGBA in little endian
void TexturePool::SetMissingTexture(uint32_t index)
{
    mDescriptorSet->Update({.binding = 0,
                             .startIndex = index,
                             .type = RHIDescriptorType::SampledImage,
                             .images = {{{.imageView = GetTextureView(mMissingTextureHandle),
                             .layout = RHITextureLayout::ShaderReadOnly
   }}}});
}
TexturePool::TexturePool(RHIDevice* device, Allocator* allocator, uint32_t max_textures) :
    mMaxTextures(max_textures), mDevice(device), mAllocator(allocator), mTextures(allocator),
    mIdleGuard(device)
{
    mDescriptorPool = mDevice->CreateDescriptorPool(
        {.bindings = {{{.type = RHIDescriptorType::SampledImage, .maxCount = max_textures}}},
         .updateAfterBind = true});
    mDescriptorSetLayout =
        mDevice->CreateDescriptorSetLayout({.bindings = {{{.count = max_textures,
                                                            .stage = RHIShaderStageBits::All,
                                                            .type = RHIDescriptorType::SampledImage}}},
                                             .updateAfterBind = true});
    mDescriptorSet = mDescriptorPool->CreateDescriptorSet(mDescriptorSetLayout, max_textures);
    mDescriptorSet->DebugSetObjectName("Texture Pool Set");
    // Create a 2x2 'missing' texture
    mMissingTextureHandle =
        Allocate({
                .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
                .extent = {2,2,1},
                .format = RHIResourceFormat::R8G8B8A8Unorm,
        });
    UploadContext ctx(device, allocator);
    ctx.Upload(GetTexture(mMissingTextureHandle), Span<const uint32_t>(kInvalidTexture).AsBytes());
    for (size_t i = 0; i < max_textures; i++)
        SetMissingTexture(i);
}
RHITexture* TexturePool::GetTexture(TexturePoolHandle handle) const
{
    return mTextures.At(handle)->Visit(
        [&](RHITextureView* const& view) { return view->GetTexture(); },
        [&](TexturePair const& pair) { return pair.first.Get(); }
    );
}
RHITextureView* TexturePool::GetTextureView(TexturePoolHandle handle) const
{
    return mTextures.At(handle)->Visit(
        [&](RHITextureView* const& view) { return view; },
        [&](TexturePair const& pair) { return pair.second.Get(); }
    );
}
TexturePoolHandle TexturePool::Allocate(RHITextureDesc const& desc, RHITextureViewDesc const& viewDesc)
{
    std::unique_lock lock(mMutex);
    auto handle = mTextures.Pop();    
    CHECK_MSG(handle < mMaxTextures, "TexturePool overflow.");
    auto texture = mDevice->CreateTexture(desc);
    auto view = texture->CreateTextureView(viewDesc);
    mDescriptorSet->Update({.binding = 0,
                             .startIndex = handle,
                             .type = RHIDescriptorType::SampledImage,
                             .images = {{{.imageView = view.Get(), .layout = RHITextureLayout::ShaderReadOnly}}}});
    *mTextures.At(handle) = TexturePair(std::move(texture), std::move(view));
    return handle;
}
TexturePoolHandle TexturePool::Allocate(RHITextureDesc const& desc)
{
    return Allocate(desc,
                    RHITextureViewDesc{.format = desc.format,
                                       .dimension = desc.dimension,
                                       .range = RHITextureSubresourceRange::Create(
                                           RHITextureAspectFlagBits::Color, 0, desc.mipLevels, 0, desc.arrayLayers)});
}
TexturePoolHandle TexturePool::Allocate(RHITextureView* view)
{
    std::unique_lock lock(mMutex);
    auto handle = mTextures.Pop();        
    CHECK_MSG(handle < mMaxTextures, "TexturePool overflow.");
    mDescriptorSet->Update({.binding = 0,
                             .startIndex = handle,
                             .type = RHIDescriptorType::SampledImage,
                             .images = {{{.imageView = view, .layout = RHITextureLayout::ShaderReadOnly}}}});
    *mTextures.At(handle) = view;
    return handle;
}
void TexturePool::Free(TexturePoolHandle handle)
{
    CHECK_MSG(handle != mMissingTextureHandle, "Attempted to free reserved texture!");
    mTextures.Free(handle);
    SetMissingTexture(handle);
}
