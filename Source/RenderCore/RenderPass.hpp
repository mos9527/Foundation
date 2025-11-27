#pragma once
#include <RHICore/Command.hpp>
#include <RHICore/Descriptor.hpp>
#include <RHICore/Device.hpp>
#include <RHICore/PipelineState.hpp>
namespace Foundation::RenderCore
{
    using namespace RHI;
    class Renderer;
    using PassHandle = size_t; // Index in the pass definitions vector
    using ResourceHandle = size_t; // Index in the resource definitions vector
    /**
     * @brief Interface for a render pass.
     */
    class RenderPass : public RHIObject
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
        virtual void Record(PassHandle self, Renderer* r, RHICommandList* cmd) = 0;
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
     * @brief Default "no-op" functor for Setup()
     */
    struct FSetupDefault
    {
        void operator()(PassHandle, Renderer*) const { /* nop */ }
    };
    /**
     * @brief Default "no-op" functor for Record()
     */
    struct FRecordDefault
    {
        void operator()(PassHandle, Renderer*, RHICommandList*) const { /* nop */ }
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
        FSetup mSetup;
        FRecord mRecord;
        FSkip mSkip;
        LambdaPass(FSetup&& setup, FRecord&& record, FSkip&& skip = {}) :
            mSetup(std::forward<FSetup>(setup)), mRecord(std::forward<FRecord>(record)),
            mSkip(std::forward<FSkip>(skip))
        {
        }
        void Setup(PassHandle self, Renderer* r) override { mSetup(self, r); }
        void Record(PassHandle self, Renderer* r, RHICommandList* cmd) override { mRecord(self, r, cmd); }
        bool IsSkipped(PassHandle self, Renderer* r) const override { return mSkip(self, r); }
    };
    /**
     * @brief Internal tracking information for a render pass in the frame graph.
     */
    struct TrackedPass
    {
        String name;
        PassHandle handle; // Index to tracked passes
        int priority{0}; // Higher priority passes are scheduled earlier
        // The queue to run this pass on
        RHIDeviceQueueType queue;
        bool used{false}; // Culled?
        // Backbuffer specializations
        Optional<RHIPipelineState::PipelineStateDesc::Attachment::Blending> backbufferRTV{}; // opt: blending mode
        Optional<int> backbufferUAV; // opt: set index
        // Uses compute shader? (not necessarily in a compute queue)
        // Should be mutually exclusive with write_backbuffer and other graphics states
        bool isComputePass{false};
        // Local size for compute/mesh shaders        
        Tuple<uint32_t, uint32_t, uint32_t> groupLocalSize{};
        size_t depth{}; // Depth in RG
        size_t ord{}; // Execution order
        size_t frameExec{}; // Last frame this pass is executed
        /* -- Resources -- */
        Vector<PassHandle> bindPasses; // Referenced, explicit pass execute-before.
        Vector<Tuple<ResourceHandle, RHIResourceAccess, RHIPipelineStage, RHITextureSubresourceRange,
                     RHITextureLayout>>
            textureUsages; // Referenced texture sub resources
        Vector<Tuple<ResourceHandle, RHIResourceAccess,
                     RHIPipelineStage>> bufferUsages; // Referenced buffers
        // Unique referenced resources (tex/buf)
        Vector<ResourceHandle> resources;
        // Unique texture views
        Vector<ResourceHandle> texviews;
        /* -- Pipeline -- */
        // Shader [path, entry point, stage]
        Vector<Tuple<String, String, RHIShaderStage>> shaders;
        // Bind points [view(tex) or buffer(buf), desc type, binding point]
        Vector<Tuple<ResourceHandle, RHIDescriptorType, String>> textureBindings, bufferBindings;
        // External Bind Sets [binding point, layout ptr, set index (set when built)]
        // Sorted lexicographically if the pipeline is built.
        Vector<Tuple<String, RHIDeviceDescriptorSetLayout*, int>> externalBindings;
        // Samplers
        Vector<Pair<ResourceHandle, String>> samplers;
        // Push Constants by [stage, offset, size]
        Vector<RHIPipelineState::PipelineStateDesc::PushConstant> pushConstants;
        // (Graphics Only) Render Target View[s], Blending Op
        Vector<Pair<ResourceHandle, RHIPipelineState::PipelineStateDesc::Attachment::Blending>> rtvs;
        // (Graphics Only) Depth Stencil View
        ResourceHandle dsv{kInvalidHandle};
        // (Graphics Only) Vertex Input assembly
        Vector<RHIPipelineState::PipelineStateDesc::VertexInput::Binding> vertexInputBindings;
        Vector<RHIVertexAttribute> vertexInputAttributes;
        /* --- */
        UniquePtr<RenderPass> pass;
        TrackedPass(Allocator* alloc, PassHandle handle, StringView name, RHIDeviceQueueType queue,
                    UniquePtr<RenderPass> renderPass, size_t priority);
        /* -- Pipeline states (built at PSO setup) -- */
        int groupIndex{}; // executionGroup index
        // All stages used in this pass
        RHIPipelineStageBits piplineStages{};
        // Pipeline states for the entire pass
        RHIDeviceScopedHandle<RHIPipelineState> pso;
        // PSO Creation parameters
        RHIPipelineState::PipelineStateDesc::Rasterizer psoRasterizer{};
        RHIPipelineState::PipelineStateDesc::DepthStencil psoDepthStencil{};
        // Layouts created by ourselves
        Vector<RHIDeviceScopedHandle<RHIDeviceDescriptorSetLayout>> descriptorLayouts;
        // Pointers. Can also contain external sets
        Vector<RHIDeviceDescriptorSetLayout*> pDescriptorLayouts;
        // Sets created by ourselves
        Vector<RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet>> descriptorSets;
        // Pointers. Can also contain external sets
        Vector<RHIDeviceDescriptorSet*> pDescriptorSets;
        // [Set Index, Set, Layout], correspond to externalBindings
        Vector<Tuple<size_t, RHIDeviceDescriptorSetLayout*>> pExternalDescriptorSets;
    };
}