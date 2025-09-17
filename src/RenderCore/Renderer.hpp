#pragma once

#include <Allocator/StackAllocator.hpp>
#include <Runtime/Native/Filesystem.hpp>
#include <Runtime/Bits/Functional.hpp>
#include <Runtime/Bits/Ranges.hpp>
#include <Runtime/Core/Core.hpp>
#include <tracy/Tracy.hpp>

#include "RHICore/Device.hpp"
#include "RHICore/Command.hpp"
#include "RHICore/Application.hpp"
/**
 * @brief Core functionalities for rendering, including the Frame Graph implementation.
 */
namespace Foundation::RenderCore {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    /* -- Constants -- */
    constexpr size_t kMaxCommandListsPerSwap = 128; // Maximum number of command lists per frame
    constexpr size_t kMaxTempResourceSemaphores = 16; // Maximum number of temporary semaphores for cross-queue barriers
    constexpr size_t kExecuteArenaSize = 16 * (1 << 20); // Maximum size of the per-frame transient arena (16MB)
    const size_t kTextureAspectCount = 3; // Color, depth, stencil @ref RHITextureAspectFlag
    const RHIPipelineStage kComputeStagesMask =
          RHIPipelineStageBits::FragmentShader |
          RHIPipelineStageBits::VertexShader |
          RHIPipelineStageBits::MeshShader |
          RHIPipelineStageBits::RayTracingShader |
          RHIPipelineStageBits::AllGraphics;
    const RHIResourceAccessBits kAllShaderWrites =
        RHIResourceAccessBits::ShaderWrite |
        RHIResourceAccessBits::RenderTargetWrite |
        RHIResourceAccessBits::DepthStencilWrite |
        RHIResourceAccessBits::TransferWrite;
    /**
     * @breif Parameters for @ref Renderer creation
     */
    struct RendererDesc {
        // Enable async compute
        bool async{ true }; 
        // Present the swapchain in Execute()
        bool present{ true }; 
    };
    using ResourceDefinition = Variant<
        RHIBufferDesc,
        RHITextureDesc,
        RHIDeviceObjectHandle<RHIBuffer>,
        RHIDeviceObjectHandle<RHITexture>,
        RHIBuffer*,
        RHITexture*
    >;
    using ResourceHandle = size_t; // Index in the resource definitions vector
    using PassHandle = size_t; // Index in the pass definitions vector
    class Renderer;
    /* -- Render Pass -- */
    /**
     * @brief Interface for a render pass.
     */
    class RenderPass : public RHIObject {
    public:
        /**
         * @brief Constructor. You may also create resources here for early setup.
         * However, access declaration must be done in Setup().
         */
        RenderPass() = default;
        /**
         * @brief Perform any setup required for this pass.
         * This may include creating resources, declaring resource accesses, etc.
         */
        virtual void Setup(PassHandle self, Renderer* r) = 0;
        /**
         * @brief Record the commands of this pass into the given command list.
         *
         * This is only executed after EndSetup() has been called,
         * and when the render graph is actually executed.
         */
        virtual void Record(PassHandle self, Renderer* r, RHICommandList* cmd) = 0;
        /**
         * @brief Determine whether this pass should be skipped during Record time
         *
         * @return Whether this pass should be skipped during execution.
         */
        virtual bool IsSkipped(PassHandle self, Renderer* r) const { return false; }
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
    struct FSkipDefault {
        bool operator()(PassHandle, Renderer*) const { return false; }
    };
    /**
     * @brief Functional wrapper for a render pass
     *
     * This is a convenience wrapper for stateless passes, and should be created via @ref Renderer::CreatePass()
     */
    template<typename FSetup,typename FRecord,typename FSkip>
    struct LambdaPass : public RenderPass {
        FSetup m_setup;
        FRecord m_record;
        FSkip m_skip;
        LambdaPass(FSetup&& setup, FRecord&& record, FSkip&& skip = {})
            : m_setup(std::forward<FSetup>(setup)),
              m_record(std::forward<FRecord>(record)),
              m_skip(std::forward<FSkip>(skip)) {}
        void Setup(PassHandle self , Renderer* r) override {
            m_setup(self, r);
        }
        void Record(PassHandle self, Renderer* r, RHICommandList* cmd) override {
            m_record(self, r, cmd);
        }
        bool IsSkipped(PassHandle self, Renderer* r) const override {
            return m_skip(self, r);
        }
    };
    /**
     * @brief Internal tracking information for a resource in the frame graph.
     */
    struct TrackedResource {
        ResourceHandle handle; // Index to tracked resources
        String name;
        ResourceDefinition desc;
        bool compute_usage{ false }; // Used in a compute pass?
        bool graphics_usage{ false }; // Used in a graphics pass?
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
            // [Only used by @ref ExecuteReleaseQueueResources]
            bool executeTempTransitionFlag{false};
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
            RHIDeviceQueueType lastOwnerQueue{RHIDeviceQueueType::Undefined};
            // [Only used by @ref ExecuteReleaseQueueResources]
            bool executeTempTransitionFlag{false};
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
                Views::all(Span<SubresourceState>{
                    lastSubresourceStates.begin() + mip_begin * mip_stride,
                    lastSubresourceStates.begin() + (mip_end + 1) * mip_stride,
                }) |
                Views::filter([=](const SubresourceState& state) {
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
            Native::Path,
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
    /**
     * @brief Renderer implementing a Frame Graph system with automatic resource tracking and synchronization.
     * 
     * For usage, see also @ref Foundation::RenderCore::Application
     */
    class Renderer {
    public:
        enum class State {
            Undefined,
            Setup,
            PostSetup,
            Execute
        };
    private:
        State m_state;
        Allocator* m_allocator{ nullptr };

        const RendererDesc m_desc{};

        uint64_t m_frame{ 0 };

        uint32_t m_frameSwaps{ 1 }; // Max frames in flight
        uint32_t m_currentSync{ 0 };
        uint32_t m_currentSwap{ 0 };

        struct Resources {
            Vector<Variant<
                RHIBuffer*,
                RHIDeviceObjectHandle<RHIBuffer>,
                RHIDeviceScopedObjectHandle<RHIBuffer>,
                RHITexture*,
                RHIDeviceObjectHandle<RHITexture>,
                RHIDeviceScopedObjectHandle<RHITexture>
                >> resources;
            Vector<Variant<
                RHITextureScopedHandle<RHITextureView>,
                RHITextureHandle<RHITextureView>
                >> views;
            Vector<RHIDeviceScopedObjectHandle<RHIDeviceSampler>> samplers;
            explicit Resources(Allocator* allocator) : resources(allocator), views(allocator), samplers(allocator) {}
            void fit(ResourceHandle handle) {
                resources.resize(std::max(resources.size(), handle + 1));
                views.resize(std::max(views.size(), handle + 1));
                samplers.resize(std::max(samplers.size(), handle + 1));
            }
        };
        UniquePtr<Resources> m_resources;

        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> m_descPool;
        RHIDeviceScopedObjectHandle<RHICommandPool> m_graphicsCmdPool{}, m_computeCmdPool{}; // Graphics, Async Compute

        struct FrameSyncObjects{
        private:
            // For async compute, we might submit multiple command buffers
            // per swap. Driver usually want them to live.                       
            Vector<RHICommandPoolScopedHandle<RHICommandList>> cmds;
            Vector<RHICommandPoolScopedHandle<RHICommandList>> comp_cmds;
        public:
            // Index of this swap
            const size_t swapIndex;
            // Whether this frame is currently being processed by the GPU
            bool isInFlight{false};
            RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> render{}, present{};
            RHIDeviceScopedObjectHandle<RHIDeviceFence> graphics_fence{}, compute_fence{};
            RHICommandList* graphics_cmd_at(size_t i, RHICommandPool* pool) {
                if (!cmds[i].IsValid())
                    cmds[i] = pool->CreateCommandList();
                cmds[i]->DebugSetObjectName(fmt::format("Swap {} Graphics Command List {}", swapIndex, i).c_str());
                return cmds[i].Get();
            }
            RHICommandList* compute_cmd_at(size_t i, RHICommandPool* pool) {
                if (!comp_cmds[i].IsValid())
                    comp_cmds[i] = pool->CreateCommandList();
                comp_cmds[i]->DebugSetObjectName(fmt::format("Swap {} Compute Command List {}", swapIndex, i).c_str());
                return comp_cmds[i].Get();
            }
            // RTV for the backbuffer
            RHITextureScopedHandle<RHITextureView> rtv{};
            // Tracked backbuffer handle
            ResourceHandle rt_handle{ kInvalidHandle };
            FrameSyncObjects(size_t swapIndex, Allocator* allocator)
                : cmds(allocator), comp_cmds(allocator), swapIndex(swapIndex)
            {
                cmds.resize(kMaxCommandListsPerSwap), comp_cmds.resize(kMaxCommandListsPerSwap);
            }
        };
        Vector<FrameSyncObjects> m_swaps;
        RHIApplicationObjectHandle<RHIDevice> m_device{};
        RHIDeviceObjectHandle<RHISwapchain> m_swapchain{};
        RHIDeviceQueue* m_graphicsQueue{}, *m_computeQueue{};

        struct Setup {            
            Vector<Vector<Pair<PassHandle, ResourceHandle>>> graph;
            Vector<TrackedPass> trackedPasses;
            Vector<TrackedResource> trackedResources;
            // [resource, view desc]
            Vector<Pair<ResourceHandle, RHITextureViewDesc>> trackedViews;
            Vector<RHIDeviceSampler::SamplerDesc> trackedSamplers;
            // [resource, ord range]
            Map<ResourceHandle, Pair<PassHandle, PassHandle>> activeResources;
            // Passes ordered by pass.ord
            Vector<PassHandle> execution;
            Map<RHIDescriptorType, uint32_t> binding_counts;
            PassHandle epilogue{ kInvalidHandle };
            // Execution grouped by queue type
            struct ExecutionGroups
            {
                const size_t group_index{}; // Index in executionGroups
                const RHIDeviceQueueType queue{};

                RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> semaphore;
                Vector<PassHandle> passes;
                // Resources used in this group
                Vector<ResourceHandle> resources;
                bool is_last_graphics = false;
                bool is_last_compute = false;
                RHIShaderStage all_stages{}; // All stages used in this group
                
                ExecutionGroups(size_t group_index, RHIDeviceQueueType queue, Allocator* allocator) :
                group_index(group_index), queue(queue), passes(allocator), resources(allocator) {}
            };
            Vector<ExecutionGroups> executionGroups;
            bool executionAnyCompute{false}, executionAnyGraphics{false};
            void add_edge(const PassHandle u, const PassHandle v,  const ResourceHandle hdl) {
                // ReSharper disable once CppDFALoopConditionNotUpdated
                while (u >= graph.size()) graph.emplace_back(graph.get_allocator());
                graph[u].emplace_back(v, hdl);
            }
            explicit Setup(Allocator* allocator) :
                graph(allocator), trackedPasses(allocator), trackedResources(allocator),
                trackedViews(allocator), trackedSamplers(allocator), activeResources(allocator),
                execution(allocator), binding_counts(allocator),
                executionGroups(allocator) {}
        };
        UniquePtr<Setup> m_setup;
        // Setup
        [[nodiscard]] ResourceHandle CreateTextureView(
            PassHandle pass, ResourceHandle res,
            RHITextureViewDesc const& desc
        ) const;
        // PostSetup
        void CullPasses(PassHandle epilogue) const;
        void BuildPipelineState(PassHandle pass);
        void FinalizeResources();
        void FinalizePSOs();
        // Temporary memory arena for execution
        ScopedArena m_executeArena;
        // Temporary allocator for execution
        // This is reset every frame, and only guaranteed to be valid during Execute state.
        StackAllocator m_executeAlloc;
        /**
         * @breif Helper to get the queue index of a queue type
         */
        uint32_t ExecuteGetQueueIndex(RHIDeviceQueueType queue) const
        {
            switch (queue)
            {
            case RHIDeviceQueueType::Undefined: return kCommandQueueTransferIgnored;
            case RHIDeviceQueueType::Graphics: return m_graphicsQueue->GetQueueIndex();
            case RHIDeviceQueueType::Compute: return m_computeQueue->GetQueueIndex();
            default:
                throw std::runtime_error("Unhandled queue type");
            }
        };
        /**
         * @param outGroups Groups need to sync with, [group index, from previous frame]
         * @note Throws if currentQueue can't be used to transition some resources
         */
        void ExecuteCheckResourceStates(
            Span<PassHandle> passes,
            RHIDeviceQueueType currentQueue,
            Vector<Pair<size_t,bool>>* outGroups = nullptr
        );
        void ExecuteBarrierSubresourceState(PassHandle pass, RHITexture* res, TrackedResource::SubresourceState& sta,
                                       RHIResourceAccess access, RHIPipelineStage stage, RHITextureLayout layout,
                                       RHICommandList* cmd);
        /**
         * @brief Executes barriers for a subresource range of a texture
         */
        void ExecuteBarrierSubresource(PassHandle pass, TrackedResource& res, RHITextureSubresourceRange const& range,
                                       RHIResourceAccess access, RHIPipelineStage stage, RHITextureLayout layout,
                                       RHICommandList* cmd);
        /**
         * @brief Executes barriers for a whole buffer
         */
        void ExecuteBarrierBuffer(PassHandle pass, TrackedResource& res, RHIResourceAccess access,
                                  RHIPipelineStage stage, RHICommandList* cmd);
        /**
         * @brief Executes all barriers for a pass
         */
        void ExecuteBarriers(TrackedPass& pass, RHICommandList* cmd);
        /**
         * @breif Acquires resources for the current group.
         */
        void ExecuteAcquireQueueResources(RHIDeviceQueueType currentQueue, size_t groupIndex, RHICommandList* cmd);
        /**
         * @breif Performs transitions that's otherwise impossible
         * (e.g. Fragment -> Compute) for the next group, and releases resources for the current group.
         */
        void ExecuteReleaseQueueResources(RHIDeviceQueueType currentQueue, size_t groupIndex, RHICommandList* cmd);
        void SetFrameSyncObjects();
        /**
         * @brief Explicitly declares that this pass will access the buffer in the specified stage with the specified access.
         *
         * This is only available at Setup time.
         *
         * This does not bind the buffer to any shader - use BindBuffer...() for that.
         */
        void DeclareBufferAccess(PassHandle pass, ResourceHandle buffer,
            RHIPipelineStage stage,
            RHIResourceAccess access = RHIResourceAccessBits::ShaderRead
        ) const;
        /**
         * @brief Declares that this pass will access the texture in the specified stage with the specified access.
         *
         * This is only available at Setup time.
         *
         * This does not bind the texture to any shader - use BindTexture...() for that.
         */
        void DeclareTextureAccess(
            PassHandle pass, ResourceHandle res,
            RHIPipelineStage stage,
            RHITextureSubresourceRange range = {},
            RHIResourceAccess access = RHIResourceAccessBits::ShaderRead,
            RHITextureLayout layout = RHITextureLayout::ShaderReadOnly
        ) const;
        RHIDeviceIdleGuard m_waitIdle; // Ensure device is idle on destruction
    public:
        Renderer(RendererDesc const& desc, RHIApplicationObjectHandle<RHIDevice> device, RHIDeviceObjectHandle<RHISwapchain> swapchain,  Allocator* allocator);

#pragma region Render Graph Setup
        /**
         * @brief Begins the setup phase of the render graph.
         *
         * @note You MUST call this before any other Create..., Bind..., or Declare... functions.
         */
        void BeginSetup();
        /**
         * @brief Create a render pass from a RenderPass* implementation and add it to the render graph.
         *
         * This is only available at Setup time.
         * 
         * @ref createPassImpl() should be generally preferred over this.
         *
         * @tparam T Type of @ref RenderPass to create.
         * @param queue Queue to prefer running this pass in - this is a hint, and might be ignored if async compute is disabled.
         */
        template<typename T, typename ...Args> requires std::is_base_of_v<RenderPass, T>
        T* CreatePassImpl(StringView name, RHIDeviceQueueType queue, Args&&... args) {
            CHECK(m_state == State::Setup);
            CHECK_MSG(queue == RHIDeviceQueueType::Graphics || queue == RHIDeviceQueueType::Compute, "Invalid queue type. Only Graphics and Compute queues are supported.");
            PassHandle handle = m_setup->trackedPasses.size();
            if (!m_desc.async)
                queue = RHIDeviceQueueType::Graphics; // Force graphics queue if async compute is disabled
            m_setup->trackedPasses.emplace_back(
                m_allocator,
                handle,
                name,
                queue,
                ConstructUniqueBase<RenderPass, T>(m_allocator, std::forward<Args>(args)...)
            );
            m_setup->epilogue = handle;
            return static_cast<T*>(m_setup->trackedPasses.back().pass.get());
        }
        /**
         * @brief Create a render pass from a Setup(Renderer*, PassHandle) and Record(Renderer*, PassHandle, RHICommandList*) lambda.
         *
         * NOTE: Prefer using this over CreatePass<T>() for stateless passes
         *
         * This can be called inside a pass's Setup() function, or after CreatePass() but before EndSetup().
         * 
         * @ref createPass() should be generally preferred over this.
         *
         * @param queue Queue to prefer running this pass in - this is a hint, and might be ignored if async compute is disabled.
         * @param setup Lambda of type `void(PassHandle self, Renderer*)` called at Setup time.
         * @param record Lambda of type `void(PassHandel self, Renderer*, RHICommandList*)` called at Record time.
         * @param skip (Optional) Lambda of type `bool(PassHandle self, Renderer*)` called at Record time
         *                        to determine whether this pass should be skipped if true. This is by default always false.
         */
        template<typename FSetup, typename FRecord, typename FSkip = FSkipDefault>
        LambdaPass<FSetup, FRecord, FSkip>* CreatePass(
            StringView name,
            RHIDeviceQueueType queue,
            FSetup&& setup,
            FRecord&& record,
            FSkip&& skip = {}
            ) {
            return CreatePassImpl<LambdaPass<FSetup, FRecord, FSkip>>(
                name, queue,
                std::forward<FSetup>(setup),
                std::forward<FRecord>(record),
                std::forward<FSkip>(skip)
            );
        }
        /**
         * @brief Create a new resource to be used in the render graph.
         *
         * This is only available at Setup time.    
         * No allocation is performed until EndSetup() is called.
         *
         * All resources created by a pass that is not culled will be created, regardless of usage.
         *
         * Resources can be imported by passing in RHIDeviceObjectHandle<RHIBuffer> or RHIDeviceObjectHandle<RHITexture>.
         * 
         * @ref createResource() should be generally preferred over this.
         *
         * @param desc Resources can be created by passing in @ref RHIBufferDesc, @ref RHITextureDesc,
         * and can be imported by passing in @ref RHIDeviceObjectHandle<RHIBuffer>, @ref RHIDeviceObjectHandle<RHITexture>,
         * or raw, pinned pointers @ref RHIBuffer*, or @ref RHITexture*
         */
        template<typename T>
        ResourceHandle CreateResource(StringView name, T const& desc) {
            CHECK(m_state == State::Setup);
            ResourceHandle index = m_setup->trackedResources.size();
            m_setup->trackedResources.emplace_back(index, name, desc, m_allocator);
            return m_setup->trackedResources.size() - 1;
        }
        /**
         * @brief Creates a sampler with the specified name and descriptor.
         *
         * This is only available at Setup time.
         * No allocation is performed until EndSetup() is called.
         * 
         * @ref createSampler() should be generally preferred over this.
         */
        [[nodiscard]] ResourceHandle CreateSampler(RHIDeviceSampler::SamplerDesc const& desc) const;
#pragma region Resource Binding
        /**
         * @brief Binds shader file path to a certain pass at a certain stage.
         *
         * This is only available at Setup time.     
         * No allocation, or parsing of shader is performed until EndSetup() is called.
         * 
         * Shaders are unique per stage, and may be omitted e.g. there's only a copy.
         */
        void BindShader(
            PassHandle pass, RHIShaderStage stage,
            StringView entry_point,
            Native::Path const& shader_path
        ) const;
        /**
         * @brief Declares a range of Push Constant used in a stage.
         *
         * This is only available at Setup time.
         * 
         * You MUST bind a valid range if Push Constants are used in shaders,
         * i.e. before calling CmdSetPushConstant()
         */
        void BindPushConstant(
            PassHandle pass, RHIShaderStage stage,
            size_t offset, size_t size
        ) const;
        /**
         * @brief Associates Vertex Input description with this pass.
         *
         * This is only available at Setup time.
         * 
         * This only applies to passes on Graphics queues. And will throw
         * otherwise.
         *
         * You MUST bind a valid VertexInput at creation time if cmd->Draw[Indexed]
         * is used.
         *
         * This can be automatically bound to the pipeline with CmdBeginGraphics().
         */
        void BindVertexInput(
            PassHandle pass,
            RHIPipelineState::PipelineStateDesc::VertexInput const& info
        ) const;
        /**
         * @brief Binds a uniform buffer to a specified binding point in a rendering pass.
         *
         * This is only available at Setup time.
         * 
         * Bind points are effectively shader variable names, which will be automatically dereferenced.
         *
         * This can be automatically bound to the pipeline with CmdSetPipeline()
         */
        void BindBufferUniform(
            PassHandle pass, ResourceHandle buffer,
            RHIPipelineStage stage, StringView bind_point
        ) const;
        /**
         * @brief Binds a storage (read-write) buffer to a specified binding point.
         *
         * This is only available at Setup time.
         * 
         * Bind points are effectively shader variable names, which will be automatically dereferenced.
         *
         * This can be automatically bound to the pipeline with CmdSetPipeline()
         */
        void BindBufferStorage(
            PassHandle pass, ResourceHandle buffer,
            RHIPipelineStage stage, StringView bind_point
        ) const;
        /**
         * @brief Binds a buffer for unordered (UAV) access from shaders (read and/or write in any order).
         * 
         * This is only available at Setup time.
         *
         * Bind points are effectively shader variable names, which will be automatically dereferenced.
         *
         * This can be automatically bound to the pipeline with CmdSetPipeline()
         */
        void BindBufferUnordered(
            PassHandle pass, ResourceHandle buffer,
            RHIPipelineStage stage, StringView bind_point
        ) const;
        /**
         * @brief Declares this pass has shaders that will read from this buffer.
         * e.g. Vertex, Index
         *
         * This by itself has no effect on binding. You need to call
         * cmd->BindVertexBuffer(), cmd->BindIndexBuffer() at Record time to
         * use the buffer.
         */
        void BindBufferShaderRead(PassHandle pass, ResourceHandle buffer, RHIPipelineStage stage) const;
        /**
         * @brief Declares that this pass will write to the buffer via copy.
         *
         * This MUST be called before calling cmd->CopyBuffer(), etc. at Record time.
         */
        void BindBufferCopyDst(PassHandle pass, ResourceHandle buffer) const;
        /**
         * @brief Declares that this pass will read from the buffer via copy
         *
         * This MUST be called before calling cmd->CopyBuffer(), etc. at Record time.
         */
        void BindBufferCopySrc(PassHandle pass, ResourceHandle buffer) const;
        /**
         * @brief Binds a sampler to the shader.
         *
         * Bind points are effectively shader variable names, which will be automatically dereferenced.
         */
        void BindTextureSampler(
            PassHandle pass, ResourceHandle sampler,
            StringView bind_point
        ) const;
        /**
         * @brief Binds a texture as a Shader Resource View (read-only sampling / fetch).
         *
         * Bind points are effectively shader variable names, which will be automatically dereferenced.
         * 
         * No view is created until EndSetup() is called.
         */
        ResourceHandle BindTextureSRV(
            PassHandle pass, ResourceHandle texture,
            StringView bind_point, RHIPipelineStage stage,
            RHITextureViewDesc const& desc
        ) const;
        /**
         * @brief Binds a texture for unordered (UAV) read-write access in shaders.
         *
         * Bind points are effectively shader variable names, which will be automatically dereferenced.
         * 
         * Declares ShaderRead | ShaderWrite access and sets layout to General (or equivalent),
         * bound as StorageImage, and creates a view.
         *
         * No view is created until EndSetup() is called.
         */
        ResourceHandle BindTextureUAV(
            PassHandle pass, ResourceHandle texture, 
            StringView bind_point, RHIPipelineStage stage,
            RHITextureViewDesc const& desc
        ) const;
        /**
         * @brief Binds a texture as a Render Target View (color attachment) for a graphics pass.
         *
         * The pass must execute on a graphics-capable queue. Multiple RTVs may be bound.
         * Returns the created/assigned view handle (auto-created if needed).
         *
         * This can be automatically bound to the pipeline with CmdBeginGraphics().
         *
         * The order of multiple render targets is the same as the insertion order of the RTVs.
         */
        ResourceHandle BindTextureRTV(
            PassHandle pass, ResourceHandle texture,
            RHITextureViewDesc const& desc
        ) const;
        /**
         * @brief Binds a texture as a Depth-Stencil View for a graphics pass.
         *
         * Only one DSV may be active per pass. Layout transitions include depth / stencil write or read.
         * Returns the created/assigned view handle (auto-created if needed).
         *
         * This can be automatically bound to the pipeline with CmdBeginGraphics().
         */
        ResourceHandle BindTextureDSV(
            PassHandle pass, ResourceHandle texture,
            RHITextureViewDesc const& desc
        ) const;
        /**
         * @brief Declares that this pass will write to the current (at Record time) swapchain backbuffer.
         *
         * @note This invalidates any other bound RTVs. With this enabled,
         * existence of other RTVs will throw at EndSetup() time.
         *
         * You can retrieve the current backbuffer RTV via DerefCurrentBackbufferView() at Record time.
         *
         * This can be automatically bound to the pipeline with CmdBeginGraphics().             
         */
        void BindBackbufferRTV(PassHandle pass) const;
        /**
         * @brief Declares that this pass will write to the texture via copy / blit (transfer destination).
         *
         * Sets TransferWrite access over the specified subresource range (all if empty).
         * No view is created; raw resource state tracking is updated.
         */
        void BindTextureCopyDst(
            PassHandle pass, ResourceHandle texture,
            RHITextureSubresourceRange const& range = {}
        ) const;
        /**
         * @brief Declares that this pass will read from the texture via copy / blit (transfer source).
         *
         * Sets TransferRead access over the specified subresource range (all if empty).
         * No view is created; raw resource state tracking is updated.
         */
        void BindTextureCopySrc(
            PassHandle pass, ResourceHandle texture,
            RHITextureSubresourceRange const& range = {}
        ) const;
        /* TODO: Push Constants */
#pragma endregion
        /**
         * @brief Finish setting up the render graph.
         *
         * The **last** created pass is used as the epilogue (final) pass,
         * and will be used to determine active passes and resource lifetimes.
         *
         * You must call this before Execute().
         */
        void EndSetup();
#pragma endregion
#pragma region Swapchain
        /**
         * @brief Get the current swapchain extents.
         */
        [[nodiscard]] RHIExtent2D GetSwapchainExtent() const {
            CHECK(m_swapchain && "Swapchain not initialized");
            return m_swapchain->m_desc.extents;
        }
        /**
        * @brief Get the current swapchain extents as a 3D extent with depth 1.
        */
        [[nodiscard]] RHIExtent3D GetSwapchainExtent3D() const {
            CHECK(m_swapchain && "Swapchain not initialized");
            auto xy = m_swapchain->m_desc.extents;
            return { xy.x,xy.y,1 };
        }
#pragma endregion
#pragma region Render Graph Runtime
        /**
         * @brief Dereference a resource handle to its underlying RHI resource.
         *
         * This should only be called inside a pass's Record() function, or after EndSetup().
         *
         * @note Passes that use DerefResource() on a resource that was not declared with
         * Bind...() or Declare...() in the pass *may* be allowed, but the behaviour is undefined as
         * the transitions will then not be tracked.
         */
        [[nodiscard]] Variant<RHIBuffer*, RHITexture*> DerefResource(const ResourceHandle handle) const
        {
            CHECK(m_resources && handle < m_resources->resources.size());
            using Tv = Variant<RHIBuffer*, RHITexture*>;
            return m_resources->resources[handle].visit(
                [](auto* ptr) -> Tv { return ptr; },
                [](auto& hdl) -> Tv { return hdl.Get(); }
                );
        }
        /**
         * @brief Dereference a texture view handle to its underlying RHI texture view.
         *
         * This should only be called inside a pass's Record() function, or after EndSetup().
         */
        [[nodiscard]] RHITextureView* DerefTextureView(const ResourceHandle handle) const
        {
            CHECK(m_resources && handle < m_resources->views.size());
            using Tv = RHITextureView*;
            return m_resources->views[handle].visit([](auto& hdl) -> Tv { return hdl.Get(); });
        }
        /**
         * @brief Dereference a sampler handle to its underlying RHI sampler.
         *
         * This should only be called inside a pass's Record() function, or after EndSetup().
         */
        [[nodiscard]] RHIDeviceSampler* DerefSampler(const ResourceHandle handle) const
        {
            CHECK(m_setup && handle < m_setup->trackedSamplers.size());
            return m_resources->samplers[handle].Get();
        }
        /**
         * @brief Dereference the automatically built pipeline state object handle associated with a given pass.
         */
        [[nodiscard]] RHIPipelineState* DerefPipelineState(const PassHandle pass) const
        {
            CHECK(m_setup && pass < m_setup->trackedPasses.size());
            auto& tpass = m_setup->trackedPasses[pass];
            return tpass.pso.Get();
        }
        /**
         * @brief Dereference the built descriptor sets associated with a given pass
         */
        [[nodiscard]] Vector<RHIDeviceDescriptorSet*> const& DerefDescriptorSets(const PassHandle pass) const
        {
            CHECK(m_setup && pass < m_setup->trackedPasses.size());
            auto& tpass = m_setup->trackedPasses[pass];
            return tpass.p_desc_sets;
        }
        /**
         * @brief Returns a pointer to the current backbuffer texture view.
         *
         * The pass must have declared BindBackbufferRTV() during setup.
         */
        [[nodiscard]] RHITextureView* DerefCurrentBackbufferView(const PassHandle pass) const
        {
            CHECK(m_state == State::Execute);
            auto& tpass = m_setup->trackedPasses[pass];            
            CHECK_MSG(tpass.write_backbuffer, "Pass {} does not write to backbuffer", tpass.name);
            return m_swaps[m_currentSwap].rtv.Get();
        }
        /**
         * @return The backing general-purpose allocator used for the Renderer
         */
        Allocator* GetAllocator() const { return m_allocator; }
#pragma endregion
#pragma region Command Recording Helpers
        /**
         * @brief Helper that retrieves the local size declared by a compute pass.
         *
         * Calling this on a non-CS bound queue is incorrect, and will throw.
         */
        [[nodiscard]] RHIExtent3D CmdGetComputeLocalSize(PassHandle pass) const;
        /**
         * @brief Helper that dispatches a compute shader with the specified **TOTAL** thread count
         * @note A valid @ref CmdSetPipeline call MUST be made before this, or the behaviour is undefined.
         *
         * This is equivalent to calling:
         * @code{.cpp}
         *     auto local_size = CmdGetComputeLocalSize(pass);
         *     cmd->Dispatch(
         *         (thread_size.x + local_size.x - 1) / local_size.x,
         *         (thread_size.y + local_size.y - 1) / local_size.y,
         *         (thread_size.z + local_size.z - 1) / local_size.z
         *     );
         * @endcode
         */
        void CmdDispatch(
            PassHandle pass, RHICommandList* cmd,
            RHIExtent3D thread_size
        ) const;
        /**
         * @brief Helper that sets the current pass's PSO and descriptor sets 
         * to the current command list.
         */
        void CmdSetPipeline(PassHandle pass, RHICommandList* cmd) const;
        /**
         * @brief Helper that pushes correct descriptor sets and PSO to the current command list, and
         * pushes correct BeginGraphics() commands with declared RTVs and DSVs to the current command list.
         *
         * @note: There MUST be a matching @ref RHICommandList::EndGraphics() call AFTER calling this, or the behaviour
         * is undefined.
         * @note: The Pipeline state MUST be set with @ref CmdSetPipeline() AFTER calling this, or the behaviour
         * is undefined.
         */
        void CmdBeginGraphics(
            PassHandle pass, RHICommandList* cmd,
            RHIExtent2D const& extent,
            Optional<RHIClearColor> const& clear_rtv = RHIClearColor{},
            Optional<RHIClearDepthStencil> const& clear_dsv = RHIClearDepthStencil{1.0f, 0u}
        );        
        /**
         * @brief Helper that sets a Push Constant range data with a single l-value.
         * @note A valid @ref CmdSetPipeline call MUST be made before this, or the behaviour is undefined.
         */
        template<typename T> void CmdSetPushConstant(PassHandle pass, RHICommandList* cmd, RHIShaderStage stage, size_t offset, T const& data) {
            CHECK(m_state == State::Execute);
            auto& tpass = m_setup->trackedPasses[pass];
            cmd->PushConstant(tpass.pso.Get(), stage, static_cast<uint32_t>(offset), { reinterpret_cast<const char*>(&data), sizeof(T) });
        }
#pragma endregion
#pragma region Debugging
        [[nodiscard]] String DbgDumpGraphviz() const;
        [[nodiscard]] String DbgDumpActivePasses() const;
        [[nodiscard]] String DbgDumpExecutionGroups() const;
#pragma endregion
#pragma region Frame Execution
        /**
         * @brief Retrieves the current state of the renderer.
         */
        [[nodiscard]] State GetState() const { return m_state; }
        /**
         * @brief Get the number of frames that can be simultaneously in-flight.
         */
        [[nodiscard]] uint32_t GetFrameSwaps() const { return m_frameSwaps; }
        /**
         * @brief Retrieves the current frame number.
         *
         * This value is monotonically increasing every time @ref EndExecute() is called,
         * and starts from 0.
         */
        [[nodiscard]] uint64_t GetFrame() const { return m_frame; }
        /**
         * @brief Retrieves the current swap index at the time of @ref ExecuteFrame().
         *
         * This value is associated with the current frame in flight.
         * It's guaranteed to be less than @ref GetFrameSwaps(), and starts from 0.
         *
         * This value is updated at @ref BeginExecute(), and remains
         * the same until the next @ref BeginExecute() call.
         */
        [[nodiscard]] uint32_t GetSwap() const { return m_currentSwap; }
        /**
         * @brief Retrieves the current synchronization index.
         *
         * This value is associated with the current synchronization primitives at the current time.
         * It's guaranteed to be less than @ref GetFrameSwaps(), and starts from 0.
         *
         * @note Values this returns can be used to index into per-swap resources,
         * and is guaranteed to be not used by the GPU with values acquired
         * after @ref BeginExecute(), and before @ref EndExecute().
         *
         * This value is updated at @ref BeginExecute(), and remains
         * the same until the next @ref BeginExecute() call.
         */
        [[nodiscard]] uint64_t GetSync() const { return m_currentSync; }
        /**
         * @brief Returns whether async compute is enabled.
         *
         * If this returns false, all passes will be executed on the graphics queue,
         * and any queue hints passed during pass creation will be ignored.
         */
        [[nodiscard]] bool IsAsyncComputeEnabled() const { return m_desc.async; }
        /**
         * @brief Returns whether the swapchain is enabled.
         *
         * If this returns false, no backbuffer will be acquired or presented,
         * and any passes that write to the backbuffer will throw at EndSetup() time.
         */
        [[nodiscard]] bool IsPresentEnabled() const { return m_desc.present; }
        /**
         * @brief Update the swapchain to a new one.
         * You must call this when the window is resized or the swapchain is invalidated.
         *
         * @note This call will block if pending GPU work exists.
         */
        void SetSwapchain(RHIDeviceObjectHandle<RHISwapchain> swapchain);
        /**
        * @brief Resets the temporary execution allocator , and waits for the possibly multi-buffered
        * next frame to finish rendering.
        *
        * See also @ref GetSync(), @ref GetSwap()
        *
        * @note This MUST be called before entering Execute* functions.
        */
        void BeginExecute();
        /**
         * @brief Executes all passes in the render graph for one frame.
         *
         * This includes recording command lists, submitting them to the appropriate queues,
         * and presenting the swapchain if enabled.
         *
         * @note This MUST be called after BeginExecuteImpl(), and before EndExecuteImpl().
         *
         * @code{.cpp}
         *  // With the above Execute... functions, a correct usage may look like this:
         *  BeginExecute();
         *  // ...Additional pre-frame logic...
         *  ExecuteFrame();
         *  // ...Additional post-frame logic...
         *  EndExecute();
         *  @endcode
         */
        void ExecuteFrame();
        /**
         * @brief Ends the execution phase, and prepares for the next frame.
         * @note This MUST be called after ExecuteFrame(), and before BeginExecuteImpl() of the next frame.
         */
        void EndExecute();
#pragma endregion
    };
    /* Functional Helpers */
    /**
     * @brief Convenient functional wrapper to create a resource
     *
     * This is equivalent to calling CreateResource(name, desc);
     *
     * @param desc Resources can be created by passing in @ref RHIBufferDesc, @ref RHITextureDesc,
     * and can be imported by passing in @ref RHIDeviceObjectHandle<RHIBuffer>, @ref RHIDeviceObjectHandle<RHITexture>,
     * or raw, pinned pointers @ref RHIBuffer*, or @ref RHITexture*
     */
    template<typename T>
    ResourceHandle createResource(Renderer* r, StringView name, T const& desc) {
        return r->CreateResource(name, desc);
    }
    /**
     * @brief Convenient functional wrapper to create a sampler
     *
     * This is equivalent to calling CreateSampler(name, desc);
     */
    inline ResourceHandle createSampler(Renderer* r, RHIDeviceSampler::SamplerDesc const& desc) {
        return r->CreateSampler(desc);
    }
    /**
     * @brief Convenient functional wrapper to create a pass from a RenderPass* implementation.
     *
     * This is equivalent to calling @ref Renderer::CreatePassImpl
     *
     * @tparam T Type of @ref RenderPass to create.
     * @param queue Queue to prefer running this pass in - this is a hint, and might be ignored if async compute is disabled.
     */
    template<typename T, typename ...Args> requires std::is_base_of_v<RenderPass, T>
    T* createPassImpl(Renderer* r, StringView name, RHIDeviceQueueType queue, Args&&... args) {
        return r->CreatePassImpl<T>(name, queue, std::forward<Args>(args)...);
    }
    /**
     * @brief Convenient functional wrapper to create a pass from Setup/Record lambdas.
     *
     * This is equivalent to calling @ref Renderer::CreatePass
     *
     * @note Avoid using Lambdas with stateful captures (i.e. capturing `this` or [&]), as resource lifetimes
     *       could be _much_ involved and unpredictable.
     *       Prefer using stateless captures (i.e. [=]) or no captures at all, unless the states are trivial, and
     *       you really know what you're doing.
     *
     * @param queue Queue to prefer running this pass in - this is a hint, and might be ignored if async compute is disabled.
     * @param setup Lambda of type `void(PassHandle self, Renderer*)` called at Setup time.
     * @param record Lambda of type `void(PassHandel self, Renderer*, RHICommandList*)` called at Record time.
     * @param skip (Optional) Lambda of type `bool(PassHandle self, Renderer*)` called at Record time
     *                        to determine whether this pass should be skipped if true. This is by default always false.
     */
    template<typename FSetup, typename FRecord, typename FSkip = FSkipDefault>
    LambdaPass<FSetup, FRecord, FSkip>* createPass(
        Renderer* r, StringView name, RHIDeviceQueueType queue,
        FSetup&& setup, FRecord&& record, FSkip&& skip = {}
    ) {
        return r->CreatePass(
            name, queue,
            std::forward<FSetup>(setup),
            std::forward<FRecord>(record),
            std::forward<FSkip>(skip)
        );
    }
    ENUM_NAME_CONV_BEGIN(Renderer::State)
        case Undefined: return "Undefined";
        case Setup: return "Setup";
        case PostSetup: return "PostSetup";
        case Execute: return "Execute";
    ENUM_NAME_CONV_END();
}
