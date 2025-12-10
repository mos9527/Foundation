#pragma once
#include <Core/Variant.hpp>
#include <RHICore/Resource.hpp>
#include "RenderPass.hpp"
namespace Foundation::RenderCore
{
    using namespace RHI;
    using namespace Core;
    using ResourceDefinition = Variant<
        RHIBufferDesc, RHITextureDesc, /* RHIAccelerationStructureDesc - not yet. Do we want to do this in RG at all? */
        RHIBuffer*, RHITexture*, RHIAccelerationStructure*
    >;
    const size_t kTextureAspectCount = 3; // Color, depth, stencil @ref RHITextureAspectFlag
    /**
     * @brief Internal tracking information for a resource in the frame graph.
     */
    struct TrackedResource
    {
        ResourceHandle handle; // Index to tracked resources
        String name;
        ResourceDefinition desc;
        bool hasComputeUsage{false}; // Used in a compute pass?
        bool hasGraphicsUsage{false}; // Used in a graphics pass?
        /* --- states --- */
        // (Buffer) Last known state
        // Transitions here are always global since granularity would be too fine. And seems
        // like drivers don't really care?
        // See Also: https://www.reddit.com/r/vulkan/comments/v2mswb/global_memory_barriers_vs_bufferimage_memory/
        // TODO: Investigate
        struct BufferState
        {
            // Last pass to write at Setup time
            PassHandle producer{kInvalidHandle};
            // Last pass to transition at Execute time
            PassHandle lastProducer{kInvalidHandle};
            // Last frame the transition was executed
            size_t lastProducedFrame{0};
            // Last queue this resource is owned by
            RHIDeviceQueueType lastOwnerQueue{RHIDeviceQueueType::Undefined};
            // [Only used by @ref ExecuteReleaseQueueResources]
            bool executeTempTransitionFlag{false};
            RHIResourceAccess access{};
            RHIPipelineStage stage{};
            void reset()
            {
                producer = kInvalidHandle;
                access = {};
                stage = {};
            }
        } lastBufferState{};

        // (Texture) Per-subresource states
        uint32_t textureLayers{0}, textureMips{0};
        struct SubresourceState
        {
            size_t layer{0}, mip{0};
            RHITextureAspectFlagBits aspect{};
            /* -- states -- */
            // Last pass to write at Setup time
            PassHandle producer{kInvalidHandle};
            // Last pass to transition at Execute time
            PassHandle lastProducer{kInvalidHandle};
            // Last frame the transition was executed
            size_t lastProducedFrame{0};
            // Last queue this resource is owned by
            RHIDeviceQueueType lastOwnerQueue{RHIDeviceQueueType::Undefined};
            // [Only used by @ref ExecuteReleaseQueueResources]
            bool executeTempTransitionFlag{false};
            RHIResourceAccess access{};
            RHIPipelineStage stage{};
            RHITextureLayout layout{};
            void reset()
            {
                producer = kInvalidHandle;
                access = {};
                stage = {};
                layout = {};
            }
            [[nodiscard]] RHITextureSubresourceRange ToRange() const;
        };
        // [mip...,
        //   layer...,
        //      aspect...]
        Vector<SubresourceState> lastSubresourceStates;
        auto GetLastSubresourceStateOf(RHITextureSubresourceRange const& range)
        {
            auto [mip_begin, mip_end] = range.GetMipLevelRange();
            auto [layer_begin, layer_end] = range.GetArrayLayerRange();
            uint32_t mip_stride = textureLayers * kTextureAspectCount;
            return Views::all(Span<SubresourceState>{
                       lastSubresourceStates.begin() + mip_begin * mip_stride,
                       lastSubresourceStates.begin() + (mip_end + 1) * mip_stride,
                   }) |
                Views::filter(
                       [=](const SubresourceState& state)
                       {
                           return (RHITextureAspectFlag(state.aspect) & range.layer.aspect) && state.mip >= mip_begin &&
                               state.mip <= mip_end && state.layer >= layer_begin && state.layer <= layer_end;
                       });
        }
        void ResetStates()
        {
            lastBufferState = {};
            for (auto& sta : lastSubresourceStates)
                sta.reset();
        }
        TrackedResource(ResourceHandle handle, StringView name, const ResourceDefinition& resourceDesc,
                        Allocator* alloc);
    };
    /**
     * @brief Helper class containing runtime resources either imported, or created by the @ref Renderer
     */
    struct ExecuteResources
    {
        Vector<Variant<RHIBuffer*, RHIDeviceScopedHandle<RHIBuffer>,
                       RHITexture*, RHIDeviceScopedHandle<RHITexture>,
                       RHIAccelerationStructure*>>
            resources;
        Vector<Variant<RHITextureScopedHandle<RHITextureView>, RHITextureHandle<RHITextureView>>> views;
        Vector<RHIDeviceScopedHandle<RHIDeviceSampler>> samplers;
        explicit ExecuteResources(Allocator* allocator) : resources(allocator), views(allocator), samplers(allocator) {}
        void fit(ResourceHandle handle)
        {
            resources.resize(std::max(resources.size(), handle + 1));
            views.resize(std::max(views.size(), handle + 1));
            samplers.resize(std::max(samplers.size(), handle + 1));
        }
    };
}