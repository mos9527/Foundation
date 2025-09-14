#pragma once
#include <ranges>
#include <utility>
#include <filesystem>
#include <RHICore/Device.hpp>
#include <RHICore/PipelineState.hpp>
#include <Allocator/StackAllocator.hpp>

#include "RenderPass.hpp"
namespace Foundation::Rendering
{
    extern const char* kShaderDescriptorFirstBindingErrorHelp;
    extern const char* kShaderDescriptorFirstSetErrorHelp;
    const size_t kTextureAspectCount = 3; // Color, depth, stencil @ref RHITextureAspectFlag
    /**
     * @brief Internal tracking information for a resource in the frame graph.
     */
    struct TrackedResource {
        ResourceHandle handle; // Index to tracked resources
        String name;
        ResourceDefinition desc;
        /* --- states --- */
        // (Buffer) Last known state
        // Transitions here are always global since granularity would be too fine. And seems
        // like drivers don't really care?
        // See Also: https://www.reddit.com/r/vulkan/comments/v2mswb/global_memory_barriers_vs_bufferimage_memory/
        // TODO: Investigate
        struct BufferState {
            // Last pass to write at Setup time
            PassHandle producer{ kInvalidHandle };
            // Last pass to transition at Execute time
            PassHandle lastExecutor{ kInvalidHandle };
            // Last frame the transition was executed
            size_t lastExecuteFrame{ 0 };
            // Last queue this resource is owned by
            RHIDeviceQueueType lastOwnerQueue{ RHIDeviceQueueType::Undefined };
            RHIResourceAccess access{};
            RHIPipelineStage stage{};
            void reset() {
                producer = kInvalidHandle;
                access = {};
                stage = {};
            }
        } lastBufferState{};

        // (Texture) Per-subresource states
        uint32_t textureLayers{ 0 }, textureMips{ 0 };
        struct SubresourceState {
            size_t layer{ 0 }, mip{ 0 };
            RHITextureAspectFlagBits aspect{};
            /* -- states -- */
            // Last pass to write at Setup time
            PassHandle producer{ kInvalidHandle };
            // Last pass to transition at Execute time
            PassHandle lastExecutor{ kInvalidHandle };
            // Last frame the transition was executed
            size_t lastExecuteFrame{ 0 };
            // Last queue this resource is owned by
            RHIDeviceQueueType lastOwnerQueue{};
            RHIResourceAccess access{};
            RHIPipelineStage stage{};
            RHITextureLayout layout{};
            void reset() {
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
        auto GetLastSubresourceStateOf(RHITextureSubresourceRange const& range) {
            auto [mip_begin, mip_end] = range.GetMipLevelRange();
            auto [layer_begin, layer_end] = range.GetArrayLayerRange();
            uint32_t mip_stride = textureLayers * kTextureAspectCount;
            return
                std::views::all(Span<SubresourceState>{
                    lastSubresourceStates.begin() + mip_begin * mip_stride,
                    lastSubresourceStates.begin() + (mip_end + 1) * mip_stride,
                }) |
                std::views::filter([=](const SubresourceState& state) {
                    return (RHITextureAspectFlag(state.aspect) & range.layer.aspect) && state.mip >= mip_begin && state.mip <= mip_end && state.layer >= layer_begin && state.layer <= layer_end;
                });
        }
        TrackedResource(
            const ResourceHandle handle,
            StringView name,
            const ResourceDefinition& resourceDesc,
            Allocator* alloc
        );
    };
    /**
     * @brief Internal tracking information for a render pass in the frame graph.
     */
    struct TrackedPass {
        String name;
        PassHandle handle; // Index to tracked passes
        // The queue to run this pass on
        RHIDeviceQueueType queue;
        bool used{ false }; // Culled?
        // Writes to the swapchain backbuffer
        // Ignores other RTVs if true
        bool write_backbuffer{ false };
        // Uses compute shader? (not necessarily in a compute queue)
        // Should be mutually exclusive with write_backbuffer and other graphics states
        bool compute_pass{ false };
        // Always be treated as a producer of the associated resources,
        // even when the access don't indicate writes.
        // Currently, this is only used for @ref Renderer::CreateTransitionPass
        bool always_produces{false};
        // Local size for compute shaders
        Tuple<uint32_t, uint32_t, uint32_t> compute_local_size{};
        size_t depth{}; // Depth in RG
        size_t ord{}; // Execution order
        /* -- resources -- */
        Vector<Tuple<
            ResourceHandle,
            RHIResourceAccess,
            RHIPipelineStage,
            RHITextureSubresourceRange,
            RHITextureLayout
            >> textureUsages; // Referenced texture sub resources
        Vector<Tuple<
            ResourceHandle,
            RHIResourceAccess,
            RHIPipelineStage
            >> bufferUsages; // Referenced buffers
        // Unique referenced resources (tex/buf)
        Vector<ResourceHandle> resources;
        // Unique texture views
        Vector<ResourceHandle> texviews;
        /* -- pipeline -- */
        // Shader [path, entry point, stage]
        Vector<Tuple<
            std::filesystem::path,
            String,
            RHIShaderStage
            >> shaders;
        // Bind points [view(tex) or buffer(buf), desc type, binding point]
        Vector<Tuple<
            ResourceHandle,
            RHIDescriptorType,
            String
            >> tex_bindings, buf_bindings;
        // Samplers
        Vector<Pair<ResourceHandle, String>> samplers;
        // Push Constants by [stage, offset, size]
        Vector<RHIPipelineState::PipelineStateDesc::PushConstant> push_constants;
        // (Graphics Only) Render Target View[s]
        Vector<ResourceHandle> rtvs;
        // (Graphics Only) Depth Stencil View
        ResourceHandle dsv{ kInvalidHandle };
        // (Graphics Only) Vertex Input assembly
        Vector<RHIPipelineState::PipelineStateDesc::VertexInput::Binding> vertex_input_bindings;
        Vector<RHIVertexAttribute> vertex_input_attributes;
        /* --- */
        UniquePtr<RenderPass> pass;
        TrackedPass(Allocator* alloc, const PassHandle handle, StringView name, RHIDeviceQueueType queue, UniquePtr<RenderPass> renderPass);
        /* -- states -- */
        size_t group_index{}; // executionGroup index
        // All stages used in this pass
        RHIPipelineStageBits pass_stages{};
        // Pipeline states for the entire pass
        RHIDeviceScopedObjectHandle<RHIPipelineState> pso;
        Vector<RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout>> desc_layouts;
        Vector<RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet>> desc_sets;
        Vector<RHIDeviceDescriptorSet*> p_desc_sets;
    };
}
