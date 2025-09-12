#include "RendererMeta.hpp"
namespace Foundation::Rendering
{
    const char* kShaderDescriptorFirstBindingErrorHelp = "This can be caused by one of the following:\n"
    "   - Parameter is optimized-out, and the binding is kept as is.\n"
    "   - Multiple entrypoints in the same shader, but they don't access the same parameters.\n"
    "Tips:\n"
    "   Try separating the entrypoints into different shader files, or sort the binding declarations"
    "so that the used bindings are continuous from 0.";

    const char* kShaderDescriptorFirstSetErrorHelp = kShaderDescriptorFirstBindingErrorHelp;

    RHITextureSubresourceRange TrackedResource::SubresourceState::ToRange() const
    {
        {
            return RHITextureSubresourceRange{
                .layer = {
                    .aspect = aspect,
                    .mip_level = static_cast<uint32_t>(mip),
                    .base_array_layer = static_cast<uint32_t>(layer),
                    .layer_count = 1
                },
                .mip_count = 1
            };
        }
    }

    TrackedResource::TrackedResource(const ResourceHandle handle, StringView name,
      const ResourceDefinition& resourceDesc, Allocator* alloc)
    : handle(handle), name(name), desc(resourceDesc), lastSubresourceStates(alloc) {
        // Init texture tracking states
        auto update_texture_desc = [&](RHITextureDesc const& desc) {
            textureLayers = desc.array_layers;
            textureMips = desc.mip_levels;
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
        desc.visit(
            [&](RHITextureDesc const& tex) { update_texture_desc(tex); },
            [&](RHIDeviceObjectHandle<RHITexture> const& tex) { update_texture_desc(tex->m_desc); },
            [&](const RHITexture* const tex) { update_texture_desc(tex->m_desc); }
        );
    }

    TrackedPass::TrackedPass(Allocator* alloc, const PassHandle handle, StringView name, RHIDeviceQueueType queue, UniquePtr<RenderPass> renderPass)
            : name(name), handle(handle), queue(queue),
            textureUsages(alloc), bufferUsages(alloc), resources(alloc), texviews(alloc),
            shaders(alloc),
            tex_bindings(alloc), buf_bindings(alloc),
            samplers(alloc),
            push_constants(alloc), rtvs(alloc),
            pass(std::move(renderPass)), desc_layouts(alloc),
            desc_sets(alloc), p_desc_sets(alloc),
            vertex_input_bindings(alloc), vertex_input_attributes(alloc)
    {
    };
}