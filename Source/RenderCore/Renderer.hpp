#pragma once
#include <Core/AllocatorStack.hpp>
#include <Core/Logging.hpp>
#include <Core/ThreadPool.hpp>
#include "RenderPass.hpp"
#include "RenderResource.hpp"
#include "Shader.hpp"
/**
 * @brief Core functionalities for rendering, including the Frame Graph implementation.
 */
namespace Foundation::RenderCore
{
    /**
     * @brief Parameters for @ref Renderer creation
     */
    struct RendererDesc
    {
        /**
         * @brief Enable Async Compute support.
         *
         * @note This requires the underlying RHI device to support an async compute queue, and
         *       the @ref Renderer having passes that's scheduled on the compute queue. (see @ref createPass).
         *
         * @note Expect fallback to single-queue operation if this is not the case, and potential
         *       performance regression if your render graph doesn't have enough compute work to
         *       balance out the synchronization overhead, or your scheduling is not optimal, e.g.
         *       overlapping bandwidth/ALU bound Graphics/Compute work together (see @ref createPassPriority).
         *
         * @note Always examine the generated execution groups (see @ref DbgDumpExecutionGroups) and _PROFILE_
         *       to verify that your render graph is scheduled as expected.
         */
        bool asyncCompute{true};
        /**
         * @brief Enable presentation support.
         *
         * @note Enabling this implies a valid @ref RHISwapchain handle is provided to the @ref Renderer
         *       on creation (see @ref Renderer::Renderer), otherwise an exception is thrown.
         */
        bool present{true};
        /**
         * @brief Number of worker threads to use for recording command lists.
         * @note Set this to 0 to disable multithreaded command recording.
         */
        uint32_t threadCount{std::max(1u, std::thread::hardware_concurrency() - 1)};
        /**
         * @brief Optional PSO cache to potentially speed up pipeline state recompilation
         *        in Setup time.
         */
        RHIPipelineStateCache* pipelineCache{nullptr};
        /**
         * @brief Enable or disable GPU profiling for the frame graph execution.
         *
         * @note When enabled, each pass will be wrapped in GPU timestamp queries,
         *       and the results can be retrieved via @ref DbgGetPassProfiles().
         *
         * @note This may introduce some overhead, especially with a large number of passes.
         *       And may not be supported by all platforms.
         */
        bool profilePasses{true};
    };
    /* -- Constants -- */
    // Maximum number of render passes per frame
    // NOTE: The limit here is mostly arbitrary - and is only used
    //       for the default priority heuristic when determining pass order.
    constexpr size_t kMaxRenderPasses = 1024;
    // Maximum number of command lists per frame
    constexpr size_t kMaxCommandListsPerThread = kMaxRenderPasses;
    // Maximum size of the per-frame transient arena (16MB)
    constexpr size_t kExecuteArenaSize = 16 * (1 << 20);
    const RHIPipelineStage kComputeStagesMask = RHIPipelineStageBits::FragmentShader |
        RHIPipelineStageBits::VertexShader | RHIPipelineStageBits::MeshShader | RHIPipelineStageBits::RayTracingShader |
        RHIPipelineStageBits::AllGraphics;
    const RHIResourceAccessBits kAllShaderWrites = RHIResourceAccessBits::ShaderWrite |
        RHIResourceAccessBits::RenderTargetWrite | RHIResourceAccessBits::DepthStencilWrite |
        RHIResourceAccessBits::TransferWrite | RHIResourceAccessBits::HostWrite;
    const RHIResourceAccessBits kAllShaderReads = RHIResourceAccessBits::ShaderRead |
        RHIResourceAccessBits::RenderTargetRead | RHIResourceAccessBits::UniformRead |
        RHIResourceAccessBits::TransferRead | RHIResourceAccessBits::HostRead;
    /**
     * @brief Renderer implementing a Frame Graph system with automatic resource tracking and synchronization.
     *
     * The Renderer is responsible for managing rendering passes, resources, and synchronization on both
     * the Graphics queue and an optional Async Compute queue (if enabled and supported).
     *
     * Do note - that the Transfer queue is not used in the Renderer. As such, you're free to use it
     * for asynchronous resource uploads.
     */
    class Renderer
    {
        /**
         * @brief Helper class containing all states pertaining to @ref Renderer's Setup phase
         */
        struct RendererSetup
        {
            Vector<Vector<Pair<PassHandle, ResourceHandle>>> graph;
            Vector<PassHandle> in;
            Vector<TrackedPass> trackedPasses;
            Vector<TrackedResource> trackedResources;
            // Backbuffer specializations
            PassHandle lastBackbufferProducer{kInvalidHandle};
            // [resource, view desc]
            Vector<Pair<ResourceHandle, RHITextureViewDesc>> trackedViews;
            Vector<RHIDeviceSampler::SamplerDesc> trackedSamplers;
            // [resource, ord range]
            Map<ResourceHandle, Pair<PassHandle, PassHandle>> activeResources;
            // Passes ordered by pass.ord
            Vector<PassHandle> execution;
            Map<RHIDescriptorType, uint32_t> bindingCounts;
            PassHandle epilogue{kInvalidHandle};
            // Execution grouped by queue type
            struct ExecutionGroups
            {
                const int groupIndex{}; // Index in executionGroups
                int graphicsGroupIndex{-1}; // Index of all unique graphics groups before this one
                int computeGroupIndex{-1}; // Index of all unique compute groups before this one
                const RHIDeviceQueueType queue{};
                Vector<PassHandle> passes;
                // Resources used in this group
                Vector<ResourceHandle> resources;
                bool isLastGraphics = false;
                bool isLastCompute = false;

                ExecutionGroups(int groupIndex, RHIDeviceQueueType queue, Allocator* allocator) :
                    groupIndex(groupIndex), queue(queue), passes(allocator), resources(allocator)
                {
                }
            };
            Vector<ExecutionGroups> executionGroups;
            bool executionAnyCompute{false}, executionAnyGraphics{false};
            int executionNumGraphicsGroups{0}, executionNumComputeGroups{0};
            void add_edge(const PassHandle u, const PassHandle v, const ResourceHandle hdl)
            {
                while (u >= graph.size())
                    graph.emplace_back(graph.get_allocator());
                graph[u].emplace_back(v, hdl);
                while (v >= in.size())
                    in.push_back(0);
                in[v]++;
            }
            explicit RendererSetup(Allocator* allocator) :
                graph(allocator), in(allocator), trackedPasses(allocator), trackedResources(allocator),
                trackedViews(allocator), trackedSamplers(allocator), activeResources(allocator), execution(allocator),
                bindingCounts(allocator), executionGroups(allocator)
            {
            }
        };

    public:
        enum class State
        {
            Undefined, // Initialized
            Setup, // During BeginSetup(), EndSetup(). No work on the GPU yet.
            PostSetup, // Safe state (with a device wait), after EndSetup(), EndExecute()
            Execute // During BeginExecute(), EndExecute()
        };

    private:
        State mState;
        Allocator* mAllocator{nullptr};

        const RendererDesc mDesc{};

        uint64_t mFrameSwapped{0}; // Frame rendered in the current Swapchain

        uint32_t mFrameSwaps{1}; // Max frames in flight
        uint32_t mCurrentSync{0};
        uint32_t mCurrentSwap{0};

        UniquePtr<ExecuteResources> mResources;
        RHIDeviceScopedHandle<RHIDeviceDescriptorPool> mDescPool;
        Mutex mDescPoolMutex;
        // Per swap primitives
        struct FrameSyncObjects
        {
            // Index of this swap
            const size_t swapIndex;
            RHIDeviceScopedHandle<RHIDeviceSemaphore> render{}, present{};
            RHIDeviceScopedHandle<RHIDeviceFence> graphicsFence{}, computeFence{};
            // Texture view for the backbuffer
            RHITextureScopedHandle<RHITextureView> view{};
            RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet> viewSet{};
            // Tracked backbuffer handle
            ResourceHandle backbuffer{kInvalidHandle};
            // [Profiling] Timestamp Query Pool
            RHIDeviceScopedHandle<RHIDeviceQueryPool> dbgQueryPool{};
            Vector<uint64_t> dbgQueryPassTimestampsResults;
            // [Profiling] Present timings
            std::chrono::steady_clock::time_point dbgSwapLastPresentTick{};
            uint64_t dbgSwapLastPresentToPresentTicks{0};
            FrameSyncObjects(size_t swapIndex, Allocator* alloc) :
                swapIndex(swapIndex), dbgQueryPassTimestampsResults(alloc)
            {
            }
        };
        RHIDeviceScopedHandle<RHIDeviceDescriptorPool> mSwapDescriptorPool;
        RHIDeviceScopedHandle<RHIDeviceDescriptorSetLayout> mSwapDescriptorSetLayout;

        Vector<FrameSyncObjects> mSwaps;
        // Semaphore for async compute
        RHIDeviceScopedHandle<RHIDeviceSemaphore> mGraphicsTimeline{}, mComputeTimeline{};
        RHIApplicationHandle<RHIDevice> mDevice{};
        RHIDeviceHandle<RHISwapchain> mSwapchain{};
        RHIDeviceQueue *mGraphicsQueue{}, *mComputeQueue{};

        UniquePtr<RendererSetup> mSetup;
        // Setup
        [[nodiscard]] ResourceHandle CreateTextureView(PassHandle pass, ResourceHandle handle,
                                                       RHITextureViewDesc const& desc) const;
        // PostSetup
        void CullPasses(PassHandle epilogue) const;
        void BuildPipelineState(PassHandle pass);
        void BuildPipelineStateAll();
        void FinalizeResources();
        void FinalizePasses();
        // Temporary memory arena for execution
        ScopedArena mExecuteArena;
        // Temporary allocator for execution
        // This is reset every frame, and only guaranteed to be valid during Execute state.
        AllocatorStack mExecuteAlloc;
        // Temporary storage for submits calls
        // This is reset every frame, and only guaranteed to be valid during Execute state.
        Vector<Pair<RHIDeviceQueueType, RHIDeviceQueue::SubmitDesc>>* mExecuteSubmits;
        // Thread pool for concurrent command list recording
        ThreadPool mExecuteThreadPool;
        struct ExecutePerThreadCommandLists
        {
            RHIDeviceScopedHandle<RHICommandPool> graphicsPool{}, computePool{};
            Vector<RHICommandPoolScopedHandle<RHICommandList>> graphicsCmds, computeCmds;
            // Resets every frame
            size_t graphicsCtr{}, computeCtr{};
            ExecutePerThreadCommandLists(RHIDevice* device, size_t maxPerThread, Allocator* alloc);
            void Reset();
            RHICommandList* AllocateGraphics();
            RHICommandList* AllocateCompute();
        };
        // [current sync][thread id]
        Vector<Vector<UniquePtr<ExecutePerThreadCommandLists>>> mExecutePerSwapCmds;
        /**
         * @param thread_id -1 for main thread, [0, kRecordThreadpoolSize] for workers
         * @note  The lifetime of the command list is only valid between @ref ExecuteBegin calls.
         *        Proper CPU-GPU synchronization is required to avoid race.
         * @return A command list allocated from the appropriate pool only used for the specified thread_id (dense)
         */
        RHICommandList* ExecuteAllocateCommandList(RHIDeviceQueueType queue, int thread_id = -1);
        /**
         * @brief Helper to get the queue index of a queue type
         */
        [[nodiscard]] uint32_t ExecuteGetQueueFamily(RHIDeviceQueueType queue) const
        {
            switch (queue)
            {
            case RHIDeviceQueueType::Undefined:
                return kCommandQueueTransferIgnored;
            case RHIDeviceQueueType::Graphics:
                return mGraphicsQueue->GetVkQueueFamily();
            case RHIDeviceQueueType::Compute:
                return mComputeQueue->GetVkQueueFamily();
            default:
                throw std::runtime_error("Unhandled queue type");
            }
        }
        using ExecuteBarrierList = Vector<Pair<Variant<RHIBuffer*, RHITexture*>, RHICommandList::TransitionDesc>>;
        using ExecuteBarrierPCmdOrPBarrierList = Variant<RHICommandList*, ExecuteBarrierList*>;
        void ExecuteBarrierSubresourceState(PassHandle pass, RHITexture* res, TrackedResource::SubresourceState& sta,
                                            RHIResourceAccess access, RHIPipelineStage stage, RHITextureLayout layout,
                                            ExecuteBarrierPCmdOrPBarrierList cmd) const;
        /**
         * @brief Executes barriers for a subresource range of a texture
         */
        void ExecuteBarrierSubresource(PassHandle pass, TrackedResource& res, RHITextureSubresourceRange const& range,
                                       RHIResourceAccess access, RHIPipelineStage stage, RHITextureLayout layout,
                                       ExecuteBarrierPCmdOrPBarrierList cmd);
        /**
         * @brief Executes barriers for a whole buffer
         */
        void ExecuteBarrierBuffer(PassHandle pass, TrackedResource& res, RHIResourceAccess access,
                                  RHIPipelineStage stage, ExecuteBarrierPCmdOrPBarrierList cmd);
        /**
         * @brief Executes all barriers for a pass
         */
        void ExecuteBarriers(TrackedPass& pass, ExecuteBarrierPCmdOrPBarrierList cmd);
        /**
         * @brief Sets backbuffer views and sync primitives
         */
        void SetFrameSyncObjects();
        /**
         * @brief Explicitly declares that this pass will access the buffer in the specified stage with the specified
         * access.
         *
         * This is only available at Setup time.
         *
         * This does not bind the buffer to any shader - use BindBuffer...() for that.
         */
        void DeclareBufferAccess(PassHandle pass, ResourceHandle handle, RHIPipelineStage stage,
                                 RHIResourceAccess access = RHIResourceAccessBits::ShaderRead) const;
        /**
         * @brief Declares that this pass will access the texture in the specified stage with the specified access.
         *
         * This is only available at Setup time.
         *
         * This does not bind the texture to any shader - use BindTexture...() for that.
         */
        void DeclareTextureAccess(PassHandle pass, ResourceHandle handle, RHIPipelineStage stage,
                                  RHITextureSubresourceRange range = {},
                                  RHIResourceAccess access = RHIResourceAccessBits::ShaderRead,
                                  RHITextureLayout layout = RHITextureLayout::ShaderReadOnly) const;
        RHIDeviceIdleGuard mWaitIdle; // Ensure device is idle on destruction
    public:
        Renderer() = delete;
        Renderer(RendererDesc const& desc, RHIApplicationHandle<RHIDevice> device,
                 RHIDeviceHandle<RHISwapchain> swapchain, Allocator* allocator);

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
         * @tparam T Type of @ref RenderPass to create.
         * @param queue Queue to prefer running this pass in - this is a hint, and might be ignored if async compute is
         * disabled.
         * @param priority Priority of this pass. Higher priority passes are scheduled earlier.
         */
        template <typename T, typename... Args>
            requires std::is_base_of_v<RenderPass, T>
        PassHandle CreatePassImpl(StringView name, RHIDeviceQueueType queue, size_t priority, Args&&... args)
        {
            CHECK(mState == State::Setup);
            CHECK_MSG(queue == RHIDeviceQueueType::Graphics || queue == RHIDeviceQueueType::Compute,
                      "Invalid queue type. Only Graphics and Compute queues are supported.");
            PassHandle handle = mSetup->trackedPasses.size();
            CHECK_MSG(handle < kMaxRenderPasses, "Exceeded maximum number of render passes ({})", kMaxRenderPasses);
            if (!mDesc.asyncCompute)
                queue = RHIDeviceQueueType::Graphics; // Force graphics queue if async compute is disabled
            mSetup->trackedPasses.emplace_back(
                mAllocator, handle, name, queue,
                ConstructUniqueBase<RenderPass, T>(mAllocator, std::forward<Args>(args)...), priority);
            mSetup->epilogue = handle;
            return handle;
        }
        /**
         * @brief Create a render pass from a Setup(Renderer*, PassHandle) and Record(Renderer*, PassHandle,
         * RHICommandList*) lambda.
         *
         * NOTE: Prefer using this over CreatePass<T>() for stateless passes
         *
         * This can be called inside a pass's Setup() function, or after CreatePass() but before EndSetup().
         *
         *
         * @param queue Queue to prefer running this pass in - this is a hint, and might be ignored if async compute is
         * disabled.
         * @param priority Priority of this pass. Higher priority passes are scheduled earlier.
         * @param setup Lambda of type `void(PassHandle self, Renderer*)` called at Setup time.
         * @param record Lambda of type `void(PassHandle self, Renderer*, RHICommandList*)` called at Record time.
         */
        template <typename FSetup, typename FRecord>
        PassHandle CreatePass(StringView name, RHIDeviceQueueType queue, size_t priority, FSetup&& setup,
                              FRecord&& record)
        {
            return CreatePassImpl<LambdaPass<FSetup, FRecord>>(name, queue, priority, std::forward<FSetup>(setup),
                                                               std::forward<FRecord>(record));
        }
        /**
         * @brief Create a new resource to be used in the render graph.
         *
         * This is only available at Setup time.
         * No allocation is performed until EndSetup() is called.
         *
         * All resources created by a pass that is not culled will be created, regardless of usage.
         *
         * Resources can be imported by passing in RHIDeviceHandle<RHIBuffer> or
         * RHIDeviceHandle<RHITexture>.
         *
         * @param desc Resources can be created by passing in @ref RHIBufferDesc, @ref RHITextureDesc, @ref
         * RHIAccelerationStructureDesc and can be imported by passing in @ref RHIBuffer*, @ref RHITexture*, @ref
         * RHIAccelerationStructure*.
         *
         * @note ALWAYS ENSURE that your IMPORTED resources OUTLIVE the @ref Renderer. There's
         *       NO reference counting or tracking of the underlying resource lifetime.
         */
        template <typename T>
        [[nodiscard]] ResourceHandle CreateResource(StringView name, T const& desc)
        {

            CHECK(mState == State::Setup);

            ResourceHandle index = mSetup->trackedResources.size();
            mSetup->trackedResources.emplace_back(index, name, desc, mAllocator);
            return mSetup->trackedResources.size() - 1;
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
         * @brief Declares an inter-pass dependency, where the *other* pass should execute-before the current pass.
         * @note  This is especially useful with passes without in-graph resource dependency, or the transitions are
         *        difficult to track within the Renderer.
         *        The ordering is enforced on recording, and on device execution.
         */
        void BindPass(PassHandle pass, PassHandle other);
        /**
         * @brief Binds shader file path to a certain pass at a certain stage.
         *
         * This is only available at Setup time.
         * No allocation, or parsing of shader is performed until EndSetup() is called.
         *
         * Shaders are unique per stage, and may be omitted e.g. there's only a copy.
         *
         * @param specializationData Binary blob of specialization data, if any. Must be bound in shader at
         *                           ID 0, offset 0.
         */
        void BindShader(PassHandle pass, RHIShaderStage stage, StringView entry_point, const char* shader_path,
                        Span<const char> specializationData = {}) const;
        /**
         * @brief Declares a range of Push Constant used in a stage.
         *
         * This is only available at Setup time.
         *
         * You MUST bind a valid range if Push Constants are used in shaders,
         * i.e. before calling CmdSetPushConstant()
         */
        void BindPushConstant(PassHandle pass, RHIShaderStage stage, size_t offset, size_t size) const;
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
        void BindVertexInput(PassHandle pass, RHIPipelineState::PipelineStateDesc::VertexInput const& info) const;
        /**
         * @brief Binds a uniform buffer to a specified binding point in a rendering pass.
         *
         * This is only available at Setup time.
         *
         * Bind points are effectively shader variable names, which will be automatically dereferenced.
         *
         * This can be automatically bound to the pipeline with CmdSetPipeline()
         */
        void BindBufferUniform(PassHandle pass, ResourceHandle buffer, RHIPipelineStage stage,
                               StringView bind_point) const;
        /**
         * @brief Binds a read-only storage buffer to a specified binding point.
         *
         * This is only available at Setup time.
         *
         * Bind points are effectively shader variable names, which will be automatically dereferenced.
         *
         * This can be automatically bound to the pipeline with CmdSetPipeline()
         */
        void BindBufferStorageRead(PassHandle pass, ResourceHandle buffer, RHIPipelineStage stage,
                                   StringView bind_point) const;
        /**
         * @brief Binds a buffer for unordered (UAV) access from shaders (read and/or write in any order).
         *
         * This is only available at Setup time.
         *
         * Bind points are effectively shader variable names, which will be automatically dereferenced.
         *
         * This can be automatically bound to the pipeline with CmdSetPipeline()
         */
        void BindBufferUnordered(PassHandle pass, ResourceHandle buffer, RHIPipelineStage stage,
                                 StringView bind_point) const;
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
        void BindTextureSampler(PassHandle pass, ResourceHandle sampler, StringView bind_point) const;
        /**
         * @brief Manually bind an existing descriptor set (layout) to the pipeline.
         *
         * @note This applies to the bind point's *whole* set. You'll need to call @ref CmdBindDescriptorSet at Record
         * time to bind the set to the pipeline.
         */
        void BindDescriptorSet(PassHandle pass, StringView bind_point, RHIDeviceDescriptorSetLayout* layout);
        /**
         * @brief Binds a texture as a Shader Resource View (read-only sampling / fetch).
         *
         * Bind points are effectively shader variable names, which will be automatically dereferenced.
         *
         * No view is created until EndSetup() is called.
         */
        ResourceHandle BindTextureSRV(PassHandle pass, ResourceHandle texture, StringView bind_point,
                                      RHIPipelineStage stage, RHITextureViewDesc const& desc) const;
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
        ResourceHandle BindTextureUAV(PassHandle pass, ResourceHandle texture, StringView bind_point,
                                      RHIPipelineStage stage, RHITextureViewDesc const& desc) const;
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
        ResourceHandle
        BindTextureRTV(PassHandle pass, ResourceHandle texture, RHITextureViewDesc const& desc,
                       RHIPipelineState::PipelineStateDesc::Attachment::Blending const& blending = {}) const;
        /**
         * @brief Binds a texture as a Depth-Stencil View for a graphics pass.
         *
         * Only one DSV may be active per pass. Layout transitions include depth / stencil write or read.
         * Returns the created/assigned view handle (auto-created if needed).
         *
         * This can be automatically bound to the pipeline with CmdBeginGraphics().
         */
        ResourceHandle BindTextureDSV(PassHandle pass, ResourceHandle texture, RHITextureViewDesc const& desc) const;
        /**
         * @breif Binds the backbuffer as the first Render Target.
         * @note  If this is used, the first RTV will always be the Backbuffer itself.
         *
         *        This can be automatically bound to the pipeline with CmdBeginGraphics().
         */
        void BindBackbufferRTV(PassHandle pass,
                               RHIPipelineState::PipelineStateDesc::Attachment::Blending const& blending = {}) const;
        /**
         * @brief Binds the backbuffer as RW access at binding 0 of set index
         */
        void BindBackbufferUAV(PassHandle pass, int set_index) const;
        /**
         * @brief Declares that this pass will write to the texture via copy / blit (transfer destination).
         *
         * Sets TransferWrite access over the specified subresource range (all if empty).
         * No view is created; raw resource state tracking is updated.
         */
        void BindTextureCopyDst(PassHandle pass, ResourceHandle texture,
                                RHITextureSubresourceRange const& range = {}) const;
        /**
         * @brief Declares that this pass will read from the texture via copy / blit (transfer source).
         *
         * Sets TransferRead access over the specified subresource range (all if empty).
         * No view is created; raw resource state tracking is updated.
         */
        void BindTextureCopySrc(PassHandle pass, ResourceHandle texture,
                                RHITextureSubresourceRange const& range = {}) const;
        /**
         * @brief Declares that this pass has shaders that will read from this texture.
         *
         * TODO: Passes cannot write to AS as of now. Figure out if we need that.
         */
        void BindAcceleartionStructureSRV(PassHandle pass, ResourceHandle as, RHIPipelineStage stage,
                                          StringView bind_point) const;
#pragma endregion
#pragma region PSO Flags
        /**
         * @brief Sets the rasterizer and depth-stencil state for a graphics pass.
         *
         * If shaders are bound to the pass, a pipeline state object will be automatically created.
         * The parameters here will be used for the PSO creation, instead of the defaults.
         */
        void PassSetRasterizerFlags(PassHandle pass,
                                    RHIPipelineState::PipelineStateDesc::Rasterizer const& rasterizer = {},
                                    RHIPipelineState::PipelineStateDesc::DepthStencil const& depth_stencil = {}) const;
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
        [[nodiscard]] RHIExtent2D GetSwapchainExtent() const
        {
            CHECK(mSwapchain && "Swapchain not initialized");
            return mSwapchain->mDesc.extents;
        }
        /**
         * @brief Get the current swapchain extents as a 3D extent with depth 1.
         */
        [[nodiscard]] RHIExtent3D GetSwapchainExtent3D() const
        {
            CHECK(mSwapchain && "Swapchain not initialized");
            auto xy = mSwapchain->mDesc.extents;
            return {xy.x, xy.y, 1};
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
         *
         * @note This returns a rvalue @ref Variant - do not use a reference to the return value!
         */
        [[nodiscard]] Variant<RHIBuffer*, RHITexture*, RHIAccelerationStructure*>
        DerefResource(const ResourceHandle handle) const
        {
            CHECK(mResources && handle < mResources->resources.size());
            using Tv = Variant<RHIBuffer*, RHITexture*, RHIAccelerationStructure*>;
            auto& res = mResources->resources[handle];
            CHECK_MSG(!res.valueless_by_exception(), "Resource handle {} is valueless", handle);
            auto ptr = res.Visit([](auto* ptr) -> Tv { return ptr; }, [](auto& hdl) -> Tv { return hdl.Get(); });
            CHECK_MSG(!ptr.valueless_by_exception(), "Resource handle {} is null", handle);
            return ptr;
        }
        /**
         * @brief Dereference a texture view handle to its underlying RHI texture view.
         *
         * This should only be called inside a pass's Record() function, or after EndSetup().
         */
        [[nodiscard]] RHITextureView* DerefTextureView(const ResourceHandle handle) const
        {
            CHECK(mResources && handle < mResources->views.size());
            using Tv = RHITextureView*;
            auto& view = mResources->views[handle];
            CHECK_MSG(!view.valueless_by_exception(), "Texture view handle {} is valueless", handle);
            return view.Visit([](auto& hdl) -> Tv { return hdl.Get(); });
        }
        /**
         * @brief Dereference a sampler handle to its underlying RHI sampler.
         *
         * This should only be called inside a pass's Record() function, or after EndSetup().
         */
        [[nodiscard]] RHIDeviceSampler* DerefSampler(const ResourceHandle handle) const
        {
            CHECK(mSetup && handle < mSetup->trackedSamplers.size());
            return mResources->samplers[handle].Get();
        }
        /**
         * @brief Dereference the automatically built pipeline state object handle associated with a given pass.
         */
        [[nodiscard]] RHIPipelineState* DerefPipelineState(const PassHandle pass) const
        {
            CHECK(mSetup && pass < mSetup->trackedPasses.size());
            auto& tpass = mSetup->trackedPasses[pass];
            return tpass.pso.Get();
        }
        /**
         * @brief Dereference the built descriptor sets associated with a given pass
         */
        [[nodiscard]] Vector<RHIDeviceDescriptorSet*> const& DerefDescriptorSets(const PassHandle pass) const
        {
            CHECK(mSetup && pass < mSetup->trackedPasses.size());
            auto& tpass = mSetup->trackedPasses[pass];
            return tpass.pDescriptorSets;
        }
        /**
         * @return The backing general-purpose allocator used for the Renderer
         */
        [[nodiscard]] Allocator* GetAllocator() const { return mAllocator; }
#pragma endregion
#pragma region Command Recording Helpers
        /**
         * @brief Helper that retrieves the local size declared by a compute pass.
         *
         * Calling this on a non-CS/Task/Mesh bound queue is incorrect, and will throw.
         */
        [[nodiscard]] RHIExtent3D CmdGetComputeLocalSize(PassHandle pass) const;
        /**
         * @brief Helper that dispatches a compute shader with the specified **THREAD** count
         * @note  To dispatch groups - use @ref RHICommandList::Dispatch as is!
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
        void CmdDispatch(PassHandle pass, RHICommandList* cmd, RHIExtent3D thread_size) const;
        /**
         * @brief Helper that sets the current pass's PSO and descriptor sets
         * to the current command list.
         */
        void CmdSetPipeline(PassHandle pass, RHICommandList* cmd) const;
        /**
         * @brief Helper that binds a single descriptor set to the current command list.
         */
        void CmdBindDescriptorSet(PassHandle pass, RHICommandList* cmd, uint32_t set_index,
                                  RHIDeviceDescriptorSet* descriptor_set) const;
        /**
         * @brief Helper that binds a single descriptor set to the set of the specified bind point.
         *
         * @note For this to work, the descriptor set MUST have been bound at Setup time with
         * @ref BindDescriptorSet().
         */
        void CmdBindDescriptorSet(PassHandle pass, RHICommandList* cmd, StringView bind_point,
                                  RHIDeviceDescriptorSet* descriptor_set) const;
        /**
         * @brief Helper that pushes correct descriptor sets and PSO to the current command list, and
         * pushes correct BeginGraphics() commands with declared RTVs and DSVs to the current command list.
         *
         * @note: There MUST be a matching @ref RHICommandList::EndGraphics() call AFTER calling this, or the behaviour
         * is undefined.
         * @note: The Pipeline state MUST be set with @ref CmdSetPipeline() AFTER calling this, or the behaviour
         * is undefined.
         */
        void CmdBeginGraphics(PassHandle pass, RHICommandList* cmd, RHIExtent2D const& extent,
                              Span<const Optional<RHIClearColor>> clear_rtv = {},
                              Optional<RHIClearDepthStencil> const& clear_dsv = RHIClearDepthStencil{0.0f, 0u});
        /**
         * @brief Helper that sets a Push Constant range data with a single l-value.
         * @note A valid @ref CmdSetPipeline call MUST be made before this, or the behaviour is undefined.
         * @note The @ref stage param must overlap with at least one stage that the PSO was created with,
         *       and must be a subset of a range declared with @ref BindPushConstant(), or the behaviour is undefined.
         */
        template <typename T>
        void CmdSetPushConstant(PassHandle pass, RHICommandList* cmd, RHIShaderStage stage, size_t offset,
                                T const& data)
        {
            CHECK(mState == State::Execute);
            auto& tpass = mSetup->trackedPasses[pass];
            cmd->PushConstant(tpass.pso.Get(), stage, static_cast<uint32_t>(offset),
                              {reinterpret_cast<const char*>(&data), sizeof(T)});
        }
#pragma endregion
#pragma region Frame Execution
        /**
         * @brief Retrieves the current state of the renderer.
         */
        [[nodiscard]] State GetState() const { return mState; }
        /**
         * @brief Get the number of frames that can be simultaneously in-flight.
         */
        [[nodiscard]] uint32_t GetFrameSwaps() const { return mFrameSwaps; }
        /**
         * @brief Retrieves the current frame number.
         *
         * This value is updated at @ref EndExecute(), and is guaranteed
         * to be monotonic.
         */
        [[nodiscard]] uint64_t GetFrame() const { return mFrameSwapped; }
        /**
         * @brief Retrieves the current swap index at the time of @ref ExecuteFrame().
         *
         * This value is associated with the current frame in flight.
         * It's guaranteed to be less than @ref GetFrameSwaps(), and starts from 0.
         *
         * @note This value is updated at @ref BeginExecute(), and remains
         *       the same until the next @ref BeginExecute() call.
         * @note See also @ref GetSync
         */
        [[nodiscard]] uint32_t GetSwap() const { return mCurrentSwap; }
        /**
         * @brief Retrieves the current synchronization index.
         *
         * This value is associated with the current synchronization primitives at the current time.
         * It's guaranteed to be less than @ref GetFrameSwaps(), and starts from 0.
         *
         * @note Values this returns can be used to index into per-swap resources,
         *       and is guaranteed to be not used by the GPU with values acquired
         *       after @ref BeginExecute(), and before @ref EndExecute().
         * @note This value is updated at @ref BeginExecute(), and remains
         *       the same until the next @ref BeginExecute() call.
         * @note This value is guaranteed to be monotonic.
         */
        [[nodiscard]] uint64_t GetSync() const { return mCurrentSync; }
        /**
         * @brief Retrieves an internal tracked pass associated with the given handle, read-only.
         */
        TrackedPass const& GetTrackedPass(PassHandle pass)
        {
            CHECK_MSG(mSetup && pass < mSetup->trackedPasses.size(), "No passes available or out of bounds");
            return mSetup->trackedPasses[pass];
        }
        /**
         * @brief Returns whether async compute is enabled.
         *
         * If this returns false, all passes will be executed on the graphics queue,
         * and any queue hints passed during pass creation will be ignored.
         */
        [[nodiscard]] bool IsAsyncComputeEnabled() const { return mDesc.asyncCompute; }
        /**
         * @brief Returns whether the swapchain is enabled.
         *
         * If this returns false, no backbuffer will be acquired or presented,
         * and any passes that write to the backbuffer will throw at EndSetup() time.
         */
        [[nodiscard]] bool IsPresentEnabled() const { return mDesc.present; }
        /**
         * @brief Update the swapchain to a new one.
         * You must call this when the window is resized or the swapchain is invalidated.
         *
         * @note This call will block if pending GPU work exists.
         *
         * @note You may want to re-create the entire @ref Renderer instead if your resources depend on
         *       the backbuffer size.
         */
        void SetSwapchain(RHIDeviceHandle<RHISwapchain> swapchain);
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
         * @note This is asynchronously executed - the function will return
         *       once all passes have been *scheduled* for recording.
         *       @ref EndExecute() is the synchronization point for the frame.
         *       Meaning - if work is required during the frame, you can do it *after*
         *       @ref ExecuteFrame() and *before* @ref EndExecute() to overlap recording work.
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
         * @brief Ends the execution phase and performs GPU submission, possibly with a @ref RHIDeviceQueue::Present
         * @note This MUST be called after ExecuteFrame(), and before BeginExecuteImpl() of the next frame.
         * @note This function will block until all command list recording is finished, but will NOT wait for GPU.
         * @throws @ref RHISwapchainResizeException if swapchain is resized and has not been recreated.
         */
        void EndExecute();
#pragma endregion
#pragma region Debugging
        [[nodiscard]] String DbgDumpGraphviz() const;
        [[nodiscard]] String DbgDumpActivePasses() const;
        [[nodiscard]] String DbgDumpExecutionGroups() const;
        /**
         * @brief Retrieves timings for all passes executed in the **last** frame
         *        associated with the specified sync index.
         *        The values are refreshed upon @ref BeginExecute() call.
         * @note  The values are laid out as follows:
         *        [pass 0 start tick] [pass 0 end tick] [pass 1 start tick] [pass 1 end tick] ...
         * @note  A span of size 0 is ALWAYS returned if no timing information is available.
         */
        Span<const uint64_t> DbgProfilePassTiming(uint64_t sync, float& resolutionNS) const;
        /**
         * @brief Retrieves the total ticks between two subsequent swapchain presents by @ref EndExecute,
         *        measured on CPU with system's high-resolution timer.
         *        The value is refreshed upon @ref EndExecute() call.
         */
        uint64_t DbgProfilePresentTiming(uint64_t sync, float& resolutionNS) const;
#pragma endregion
    };
    ENUM_NAME_CONV_BEGIN(Renderer::State)
    ENUM_NAME(Undefined);
    ENUM_NAME(Setup);
    ENUM_NAME(PostSetup);
    ENUM_NAME(Execute);
    ENUM_NAME_CONV_END();
} // namespace Foundation::RenderCore
