namespace Foundation::RenderCore
{
    RHITextureSubresourceRange TrackedResource::SubresourceState::ToRange() const
    {
        {
            return RHITextureSubresourceRange{.layer = {.aspect = aspect,
                                                        .mipLevel = static_cast<uint32_t>(mip),
                                                        .baseArrayLayer = static_cast<uint32_t>(layer),
                                                        .layerCount = 1},
                                              .mipCount = 1};
        }
    }

    TrackedResource::TrackedResource(const ResourceHandle handle, StringView name, const ResourceDefinition& resourceDesc,
                                     Allocator* alloc) :
        handle(handle), name(name), desc(resourceDesc), lastSubresourceStates(alloc)
    {
        // Init texture tracking states
        auto update_texture_desc = [&](RHITextureDesc const& texture_desc)
        {
            textureLayers = texture_desc.arrayLayers;
            textureMips = texture_desc.mipLevels;
            lastSubresourceStates.resize(textureMips * textureLayers * kTextureAspectCount);
            for (uint32_t mip = 0; mip < textureMips; ++mip)
            {
                for (uint32_t layer = 0; layer < textureLayers; ++layer)
                {
                    for (uint32_t aspect = 0; aspect < kTextureAspectCount; ++aspect)
                    {
                        uint32_t i = mip * (textureLayers * kTextureAspectCount) + layer * kTextureAspectCount + aspect;
                        auto& state = lastSubresourceStates[i];
                        state.aspect = RHITextureAspectFlag(1u << aspect);
                        state.mip = mip, state.layer = layer;
                    }
                }
            }
        };
        desc.VisitDefault([&](RHITextureDesc const& tex) { update_texture_desc(tex); },
                   [&](RHIDeviceHandle<RHITexture> const& tex) { update_texture_desc(tex->mDesc); },
                   [&](const RHITexture* const tex) { update_texture_desc(tex->mDesc); }
                );
    }

}