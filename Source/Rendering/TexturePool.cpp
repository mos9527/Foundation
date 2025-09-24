#include "TexturePool.hpp"
#include <Rendering/UploadContext.hpp>
using namespace Foundation::Rendering;
const uint32_t kInvalidTexture[4] = {0xFFFF00FF, 0x00000000, 0x00000000, 0xFFFF00FF}; // RGBA in little endian
void TexturePool::SetMissingTexture(uint32_t index)
{
    m_descriptorSet->Update({.binding = 0,
                             .startIndex = index,
                             .type = RHIDescriptorType::SampledImage,
                             .images = {{{.image_view = GetTextureView(m_missingTextureHandle),
                             .layout = RHITextureLayout::ShaderReadOnly
   }}}});
}
TexturePool::TexturePool(RHIDevice* device, Allocator* allocator, uint32_t max_textures) :
    m_maxTextures(max_textures), m_device(device), m_allocator(allocator), m_textures(allocator),
    m_idleGuard(device)
{
    m_descriptorPool = m_device->CreateDescriptorPool(
        {.bindings = {{{.type = RHIDescriptorType::SampledImage, .max_count = max_textures}}},
         .update_after_bind = true});
    m_descriptorSetLayout =
        m_device->CreateDescriptorSetLayout({.bindings = {{{.count = max_textures,
                                                            .stage = RHIShaderStageBits::All,
                                                            .type = RHIDescriptorType::SampledImage}}},
                                             .update_after_bind = true});
    m_descriptorSet = m_descriptorPool->CreateDescriptorSet(m_descriptorSetLayout, max_textures);
    // Create a 2x2 'missing' texture
    m_missingTextureHandle =
        Allocate({
                .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
                .extent = {2,2,1},
                .format = RHIResourceFormat::R8G8B8A8_UNORM,
        });
    UploadContext ctx(device, allocator);
    ctx.Upload(GetTexture(m_missingTextureHandle), Span<const uint32_t>(kInvalidTexture).AsBytes());
    for (size_t i = 0; i < max_textures; i++)
        SetMissingTexture(i);
}
RHITexture* TexturePool::GetTexture(TexturePoolHandle handle) const
{
    return m_textures.at(handle).visit(
        [&](RHITextureView* const& view) { return view->GetTexture(); },
        [&](TexturePair const& pair) { return pair.first.Get(); }
    );
}
RHITextureView* TexturePool::GetTextureView(TexturePoolHandle handle) const
{
    return m_textures.at(handle).visit(
        [&](RHITextureView* const& view) { return view; },
        [&](TexturePair const& pair) { return pair.second.Get(); }
    );
}
TexturePoolHandle TexturePool::Allocate(RHITextureDesc const& desc, RHITextureViewDesc const& viewDesc)
{
    auto [handle, resource] = m_textures.pop_pair();
    CHECK_MSG(handle < m_maxTextures, "TexturePool overflow.");
    auto texture = m_device->CreateTexture(desc);
    auto view = texture->CreateTextureView(viewDesc);
    m_descriptorSet->Update({.binding = 0,
                             .startIndex = handle,
                             .type = RHIDescriptorType::SampledImage,
                             .images = {{{.image_view = view.Get(), .layout = RHITextureLayout::ShaderReadOnly}}}});
    resource = TexturePair(std::move(texture), std::move(view));
    return handle;
}
TexturePoolHandle TexturePool::Allocate(RHITextureDesc const& desc)
{
    return Allocate(desc,
                    RHITextureViewDesc{.format = desc.format,
                                       .dimension = desc.dimension,
                                       .range = RHITextureSubresourceRange::Create(
                                           RHITextureAspectFlagBits::Color, 0, desc.mip_levels, 0, desc.array_layers)});
}
TexturePoolHandle TexturePool::Allocate(RHITextureView* view)
{
    auto [handle, resource] = m_textures.pop_pair();
    CHECK_MSG(handle < m_maxTextures, "TexturePool overflow.");
    m_descriptorSet->Update({.binding = 0,
                             .startIndex = handle,
                             .type = RHIDescriptorType::SampledImage,
                             .images = {{{.image_view = view, .layout = RHITextureLayout::ShaderReadOnly}}}});
    resource = view;
    return handle;
}
void TexturePool::Free(TexturePoolHandle handle)
{
    CHECK_MSG(handle != m_missingTextureHandle, "Attempted to free reserved texture!");
    m_textures.free(handle);
    SetMissingTexture(handle);
}
