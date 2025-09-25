#pragma once
#include <RHICore/Command.hpp>
#include <RHICore/Descriptor.hpp>
#include <RHICore/Device.hpp>
#include <RHICore/PipelineState.hpp>
#include <Native/Filesystem.hpp>
namespace Foundation::RenderCore
{
    using namespace RHI;
    class Renderer;
    using PassHandle = size_t; // Index in the pass definitions vector
    using ResourceHandle = size_t; // Index in the resource definitions vector
    /**
     * @brief Interface for a render pass.
     */
    class RenderPass : public RHI::RHIObject
    {
    public:
        /**
         * @brief Constructor. You may also create resources here for early setup.
         * However, access declaration must be done in Setup().
         */
        RenderPass() = default;
        /**
         * @brief Perform any setup required for this pass.
         * This may include creating resources, declaring resource accesses, etc.
         *
         * @note This is only executed during the Setup phase of the render graph,
         * and is always called from the main (renderer's) thread.
         */
        virtual void Setup(PassHandle self, Renderer* r) = 0;
        /**
         * @brief Record the commands of this pass into the given command list.
         *
         * This is only executed after EndSetup() has been called,
         * and when the render graph is actually executed.
         *
         * @note This may be called from multiple threads concurrently, and thus
         * should ensure thread safety if accessing shared data.
         */
        virtual void Record(PassHandle self, Renderer* r, RHI::RHICommandList* cmd) = 0;
        /**
         * @brief Determine whether this pass should be skipped during Record time
         *
         * @return Whether this pass should be skipped during execution.
         *
         * This is only executed after EndSetup() has been called,
         * and when the render graph is actually executed.
         *
         * @note This is always called from the main (renderer's) thread.
         */
        virtual bool IsSkipped(PassHandle self, Renderer* r) const { return false; }
    };
    /**
     * @brief Default "no-op" functor for Record()
     */
    struct FRecordDefault
    {
        void operator()(PassHandle, Renderer*, RHI::RHICommandList*) const { /* nop */ }
    };
    /**
     * @brief Default "not skipped" functor for IsSkipped()
     */
    struct FSkipDefault
    {
        bool operator()(PassHandle, Renderer*) const { return false; }
    };
    /**
     * @brief Functional wrapper for a render pass
     *
     * This is a convenience wrapper for stateless passes, and should be created via @ref Renderer::CreatePass()
     */
    template <typename FSetup, typename FRecord, typename FSkip>
    struct LambdaPass : public RenderPass
    {
        FSetup m_setup;
        FRecord m_record;
        FSkip m_skip;
        LambdaPass(FSetup&& setup, FRecord&& record, FSkip&& skip = {}) :
            m_setup(std::forward<FSetup>(setup)), m_record(std::forward<FRecord>(record)),
            m_skip(std::forward<FSkip>(skip))
        {
        }
        void Setup(PassHandle self, Renderer* r) override { m_setup(self, r); }
        void Record(PassHandle self, Renderer* r, RHI::RHICommandList* cmd) override { m_record(self, r, cmd); }
        bool IsSkipped(PassHandle self, Renderer* r) const override { return m_skip(self, r); }
    };
    /**
     * @brief Internal tracking information for a render pass in the frame graph.
     */
    struct TrackedPass
    {
        String name;
        PassHandle handle; // Index to tracked passes
        size_t priority{0}; // Higher priority passes are scheduled earlier
        // The queue to run this pass on
        RHIDeviceQueueType queue;
        bool used{false}; // Culled?
        // Writes to the swapchain backbuffer
        // Ignores other RTVs if true
        bool write_backbuffer{false};
        // Uses compute shader? (not necessarily in a compute queue)
        // Should be mutually exclusive with write_backbuffer and other graphics states
        bool compute_pass{false};
        // Always be treated as a producer of the associated resources,
        // even when the access don't indicate writes.
        // Currently, this is only used for @ref Renderer::CreateTransitionPass
        bool always_produces{false};
        // Local size for compute shaders
        Tuple<uint32_t, uint32_t, uint32_t> compute_local_size{};
        size_t depth{}; // Depth in RG
        size_t ord{}; // Execution order
        /* -- resources -- */
        Vector<Tuple<ResourceHandle, RHIResourceAccess, RHIPipelineStage, RHITextureSubresourceRange,
                     RHITextureLayout>>
            textureUsages; // Referenced texture sub resources
        Vector<Tuple<ResourceHandle, RHIResourceAccess,
                     RHIPipelineStage>> bufferUsages; // Referenced buffers
        // Unique referenced resources (tex/buf)
        Vector<ResourceHandle> resources;
        // Unique texture views
        Vector<ResourceHandle> texviews;
        /* -- pipeline -- */
        // Shader [path, entry point, stage]
        Vector<Tuple<Native::Path, String, RHIShaderStage>> shaders;
        // Bind points [view(tex) or buffer(buf), desc type, binding point]
        Vector<Tuple<ResourceHandle, RHIDescriptorType, String>> tex_bindings, buf_bindings;
        // External Bind Sets [Ptr, Layout Ptr, binding point]
        Vector<Tuple<RHIDeviceDescriptorSet*, RHIDeviceDescriptorSetLayout*, String>> external_sets;
        // Samplers
        Vector<Pair<ResourceHandle, String>> samplers;
        // Push Constants by [stage, offset, size]
        Vector<RHIPipelineState::PipelineStateDesc::PushConstant> push_constants;
        // (Graphics Only) Render Target View[s]
        Vector<ResourceHandle> rtvs;
        // (Graphics Only) Depth Stencil View
        ResourceHandle dsv{kInvalidHandle};
        // (Graphics Only) Vertex Input assembly
        Vector<RHIPipelineState::PipelineStateDesc::VertexInput::Binding> vertex_input_bindings;
        Vector<RHIVertexAttribute> vertex_input_attributes;
        /* --- */
        UniquePtr<RenderPass> pass;
        TrackedPass(Allocator* alloc, const PassHandle handle, StringView name, RHIDeviceQueueType queue,
                    UniquePtr<RenderPass> renderPass, size_t priority);
        /* -- states -- */
        size_t group_index{}; // executionGroup index
        // All stages used in this pass
        RHIPipelineStageBits pass_stages{};
        // Pipeline states for the entire pass
        RHIDeviceScopedObjectHandle<RHIPipelineState> pso;
        // Layouts created by ourselves
        Vector<RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout>> desc_layouts;
        // Pointers. Can also contain external sets
        Vector<RHIDeviceDescriptorSetLayout*> p_desc_layouts;
        // Sets created by ourselves
        Vector<RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet>> desc_sets;
        // Pointers. Can also contain external sets
        Vector<RHIDeviceDescriptorSet*> p_desc_sets;
        // [Set Index, Set, Layout]
        Vector<Tuple<size_t, RHIDeviceDescriptorSet*, RHIDeviceDescriptorSetLayout*>> external_desc_sets;
    };
}