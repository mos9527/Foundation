#pragma once
#include <Async/ThreadPool.hpp>
#include <Core/StackAllocator.hpp>
#include <RHICore/Application.hpp>

#include "RenderPass.hpp"
#include "RenderResource.hpp"
/**
 * @brief Core functionalities for rendering, including the Frame Graph implementation.
 */
namespace Foundation::RenderCore
{
    /* -- Constants -- */
    // Maximum number of render passes per frame
    // NOTE: The limit here is mostly arbitrary - and is only used
    //       for the default priority heuristic when determining pass order.
    constexpr size_t kMaxRenderPasses = 1024;
    constexpr size_t kRecordThreadPoolSize = 4; // Threads to record command lists concurrently
    constexpr size_t kMaxCommandListsPerThread = kMaxRenderPasses; // Maximum number of command lists per frame    
    constexpr size_t kExecuteArenaSize = 16 * (1 << 20); // Maximum size of the per-frame transient arena (16MB)
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
     * @brief Helper class containing all states pertaining to @ref Renderer's Setup phase
     */
    struct RendererSetup
    {
        Vector<Vector<Pair<PassHandle, ResourceHandle>>> graph;
        Vector<TrackedPass> trackedPasses;
        Vector<TrackedResource> trackedResources;
        // Backbuffer producer
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
            RHIPipelineStage allStages{}; // All stages used in this group

            ExecutionGroups(int groupIndex, RHIDeviceQueueType queue, Allocator* allocator) :
                groupIndex(groupIndex), queue(queue), passes(allocator), resources(allocator)
            {
            }
        };
        Vector<ExecutionGroups> executionGroups;
        bool executionAnyCompute{false}, executionAnyGraphics{false};
        void add_edge(const PassHandle u, const PassHandle v, const ResourceHandle hdl)
        {
            while (u >= graph.size())
                graph.emplace_back(graph.get_allocator());
            graph[u].emplace_back(v, hdl);
        }
        explicit RendererSetup(Allocator* allocator) :
            graph(allocator), trackedPasses(allocator), trackedResources(allocator), trackedViews(allocator),
            trackedSamplers(allocator), activeResources(allocator), execution(allocator), bindingCounts(allocator),
            executionGroups(allocator)
        {
        }
    };
    /**
     * @brief Parameters for @ref Renderer creation
     */
    struct RendererDesc
    {
        // Enable async compute
        bool async{true};
        // Present the swapchain in Execute()
        bool present{true};
    };
    class Renderer;
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
    public:
        enum class State
        {
            Undefined,
            Setup,
            PostSetup,
            Execute
        };

    private:
        State mState;
        Allocator* mAllocator{nullptr};

        const RendererDesc mDesc{};

        uint64_t mFrame{0};

        uint32_t mFrameSwaps{1}; // Max frames in flight
        uint32_t mCurrentSync{0};
        uint32_t mCurrentSwap{0};

        UniquePtr<ExecuteResources> mResources;
        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> mDescPool;
        struct FrameSyncObjects
        {
            // Index of this swap
            const size_t swapIndex;
            RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> render{}, present{};
            RHIDeviceScopedObjectHandle<RHIDeviceFence> graphicsFence{}, computeFence{};
            // RTV for the backbuffer
            RHITextureScopedHandle<RHITextureView> rtv{};
            // Tracked backbuffer handle
            ResourceHandle backbuffer{kInvalidHandle};
            FrameSyncObjects(size_t swapIndex) : swapIndex(swapIndex) {};
        };
        Vector<FrameSyncObjects> mSwaps;
        // Semaphore for async compute
        RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> mGraphicsTimeline{}, mComputeTimeline{};
        RHIApplicationObjectHandle<RHIDevice> mDevice{};
        RHIDeviceObjectHandle<RHISwapchain> mSwapchain{};
        RHIDeviceQueue *mGraphicsQueue{}, *mComputeQueue{};

        UniquePtr<RendererSetup> mSetup;
        // Setup
        [[nodiscard]] ResourceHandle CreateTextureView(PassHandle pass, ResourceHandle handle,
                                                       RHITextureViewDesc const& desc) const;
        // PostSetup
        void CullPasses(PassHandle epilogue) const;
        void BuildPipelineState(PassHandle pass);
        void FinalizeResources();
        void FinalizePSOs();
        // Temporary memory arena for execution
        ScopedArena mExecuteArena;
        // Temporary allocator for execution
        // This is reset every frame, and only guaranteed to be valid during Execute state.
        StackAllocator mExecuteAlloc;
        // Thread pool for concurrent command list recording
        Async::ThreadPool mExecuteThreadPool;
        struct ExecutePerThreadCommandLists
        {
            RHIDeviceScopedObjectHandle<RHICommandPool> graphicsPool{}, computePool{};
            Vector<RHICommandPoolScopedHandle<RHICommandList>> graphicsCmds, computeCmds;
            // Resets every frame
            size_t graphicsCtr{}, computeCtr{};
            ExecutePerThreadCommandLists(RHIDevice* device, size_t maxPerThread, Allocator* alloc);
            void Reset();
            RHICommandList* AllocateGraphics(int thread_id);
            RHICommandList* AllocateCompute(int thread_id);
        };
        // [current sync][thread id]
        Vector<Vector<UniquePtr<ExecutePerThreadCommandLists>>> mExecutePerSwapCmds;
        /**
         * @param thread_id -1 for main thread, [0, kRecordThreadpoolSize] for workers
         * @return A command list allocated from the appropriate pool only used for the specified thread_id (dense)
         */
        RHICommandList* ExecuteAllocateCommandList(RHIDeviceQueueType queue, int thread_id);
        /**
         * @brief Helper to get the queue index of a queue type
         */
        [[nodiscard]] uint32_t ExecuteGetQueueIndex(RHIDeviceQueueType queue) const
        {
            switch (queue)
            {
            case RHIDeviceQueueType::Undefined:
                return kCommandQueueTransferIgnored;
            case RHIDeviceQueueType::Graphics:
                return mGraphicsQueue->GetQueueIndex();
            case RHIDeviceQueueType::Compute:
                return mComputeQueue->GetQueueIndex();
            default:
                throw std::runtime_error("Unhandled queue type");
            }
        };
        void ExecuteBarrierSubresourceState(PassHandle pass, RHITexture* res, TrackedResource::SubresourceState& sta,
                                            RHIResourceAccess access, RHIPipelineStage stage, RHITextureLayout layout,
                                            RHICommandList* cmd) const;
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
         * @brief Acquires resources for the current group.
         */
        void ExecuteAcquireQueueResources(RHIDeviceQueueType currentQueue, size_t groupIndex, RHICommandList* cmd);
        /**
         * @brief Performs transitions that's otherwise impossible
         * (e.g. Fragment -> Compute) for the next group, and releases resources for the current group.
         */
        void ExecuteReleaseQueueResources(RHIDeviceQueueType currentQueue, size_t groupIndex, RHICommandList* cmd);
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
        Renderer(RendererDesc const& desc, RHIApplicationObjectHandle<RHIDevice> device,
                 RHIDeviceObjectHandle<RHISwapchain> swapchain, Allocator* allocator);

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
         * @param queue Queue to prefer running this pass in - this is a hint, and might be ignored if async compute is
         * disabled.
         * @param priority Priority of this pass. Higher priority passes are scheduled earlier.
         */
        template <typename T, typename... Args>
            requires std::is_base_of_v<RenderPass, T>
        T* CreatePassImpl(StringView name, RHIDeviceQueueType queue, size_t priority, Args&&... args)
        {
            CHECK(mState == State::Setup);
            CHECK_MSG(queue == RHIDeviceQueueType::Graphics || queue == RHIDeviceQueueType::Compute,
                      "Invalid queue type. Only Graphics and Compute queues are supported.");
            PassHandle handle = mSetup->trackedPasses.size();
            CHECK_MSG(handle < kMaxRenderPasses, "Exceeded maximum number of render passes ({})", kMaxRenderPasses);
            if (!mDesc.async)
                queue = RHIDeviceQueueType::Graphics; // Force graphics queue if async compute is disabled
            mSetup->trackedPasses.emplace_back(
                mAllocator, handle, name, queue,
                ConstructUniqueBase<RenderPass, T>(mAllocator, std::forward<Args>(args)...), priority);
            mSetup->epilogue = handle;
            return static_cast<T*>(mSetup->trackedPasses.back().pass.get());
        }
        /**
         * @brief Create a render pass from a Setup(Renderer*, PassHandle) and Record(Renderer*, PassHandle,
         * RHICommandList*) lambda.
         *
         * NOTE: Prefer using this over CreatePass<T>() for stateless passes
         *
         * This can be called inside a pass's Setup() function, or after CreatePass() but before EndSetup().
         *
         * @ref createPass() should be generally preferred over this.
         *
         * @param queue Queue to prefer running this pass in - this is a hint, and might be ignored if async compute is
         * disabled.
         * @param priority Priority of this pass. Higher priority passes are scheduled earlier.
         * @param setup Lambda of type `void(PassHandle self, Renderer*)` called at Setup time.
         * @param record Lambda of type `void(PassHandel self, Renderer*, RHICommandList*)` called at Record time.
         * @param skip (Optional) Lambda of type `bool(PassHandle self, Renderer*)` called at Record time
         *                        to determine whether this pass should be skipped if true. This is by default always
         * false.
         */
        template <typename FSetup, typename FRecord, typename FSkip = FSkipDefault>
        LambdaPass<FSetup, FRecord, FSkip>* CreatePass(StringView name, RHIDeviceQueueType queue, size_t priority,
                                                       FSetup&& setup, FRecord&& record, FSkip&& skip = {})
        {
            return CreatePassImpl<LambdaPass<FSetup, FRecord, FSkip>>(
                name, queue, priority, std::forward<FSetup>(setup), std::forward<FRecord>(record),
                std::forward<FSkip>(skip));
        }
        /**
         * @brief Create a new resource to be used in the render graph.
         *
         * This is only available at Setup time.
         * No allocation is performed until EndSetup() is called.
         *
         * All resources created by a pass that is not culled will be created, regardless of usage.
         *
         * Resources can be imported by passing in RHIDeviceObjectHandle<RHIBuffer> or
         * RHIDeviceObjectHandle<RHITexture>.
         *
         * @ref createResource() should be generally preferred over this.
         *
         * @param desc Resources can be created by passing in @ref RHIBufferDesc, @ref RHITextureDesc,
         * and can be imported by passing in @ref RHIDeviceObjectHandle<RHIBuffer>, @ref
         * RHIDeviceObjectHandle<RHITexture>, or raw, pinned pointers @ref RHIBuffer*, or @ref RHITexture*
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
         * @brief Binds shader file path to a certain pass at a certain stage.
         *
         * This is only available at Setup time.
         * No allocation, or parsing of shader is performed until EndSetup() is called.
         *
         * Shaders are unique per stage, and may be omitted e.g. there's only a copy.
         */
        void BindShader(PassHandle pass, RHIShaderStage stage, StringView entry_point,
                        Native::Path const& shader_path) const;
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
        void BindTextureSampler(PassHandle pass, ResourceHandle sampler, StringView shader_name) const;
        /**
         * @brief Manually bind an existing descriptor set to the pipeline.
         *
         * Effectively, the bind point would be ignored by the PSO build process,
         * and the descriptor set would be bound at the set belonging to the bind point.
         *
         * @note The bind point is only used to determine the set index.
         * The binding index themselves is then _not_ checked by the @ref Renderer,
         * therefore the shader and the descriptor set must guarantee match.
         *
         * This can be automatically bound to the pipeline with CmdSetPipeline()
         */
        void BindDescriptorSet(PassHandle pass, StringView bind_point, RHIDeviceDescriptorSet* descriptor_set,
                               RHIDeviceDescriptorSetLayout* layout);
        /**
         * @brief Binds a texture as a Shader Resource View (read-only sampling / fetch).
         *
         * Bind points are effectively shader variable names, which will be automatically dereferenced.
         *
         * No view is created until EndSetup() is called.
         */
        ResourceHandle BindTextureSRV(PassHandle pass, ResourceHandle texture, StringView shader_name,
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
        ResourceHandle BindTextureUAV(PassHandle pass, ResourceHandle texture, StringView shader_name,
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
        ResourceHandle BindTextureRTV(PassHandle pass, ResourceHandle texture, RHITextureViewDesc const& desc, RHIPipelineState::PipelineStateDesc::Attachment::Blending const& blending = {}) const;
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
         * @brief Declares that this pass will write to the current (at Record time) swapchain backbuffer.
         *
         * @note This invalidates any other bound RTVs. With this enabled,
         * existence of other RTVs will throw at EndSetup() time.
         *
         * You can retrieve the current backbuffer RTV via DerefCurrentBackbufferView() at Record time.
         *
         * This can be automatically bound to the pipeline with CmdBeginGraphics().
         */
        void BindBackbufferRTV(PassHandle pass, RHIPipelineState::PipelineStateDesc::Attachment::Blending const& blending = {}) const;
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
        /* TODO: Push Constants */
#pragma endregion
#pragma region PSO Flags
        /**
        * @brief Sets the rasterizer and depth-stencil state for a graphics pass.
        * 
        * If shaders are bound to the pass, a pipeline state object will be automatically created.
        * The parameters here will be used for the PSO creation, instead of the defaults.
        */
        void PassSetRasterizerFlags(
            PassHandle pass, 
            RHIPipelineState::PipelineStateDesc::Rasterizer const& rasterizer = {},
            RHIPipelineState::PipelineStateDesc::DepthStencil const& depth_stencil = {}
        ) const;
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
         */
        [[nodiscard]] Variant<RHIBuffer*, RHITexture*> DerefResource(const ResourceHandle handle) const
        {
            CHECK(mResources && handle < mResources->resources.size());
            using Tv = Variant<RHIBuffer*, RHITexture*>;
            return mResources->resources[handle].Visit([](auto* ptr) -> Tv { return ptr; },
                                                        [](auto& hdl) -> Tv { return hdl.Get(); });
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
            return mResources->views[handle].Visit([](auto& hdl) -> Tv { return hdl.Get(); });
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
         * @brief Returns a pointer to the current backbuffer texture view.
         *
         * The pass must have declared BindBackbufferRTV() during setup.
         */
        [[nodiscard]] RHITextureView* DerefCurrentBackbufferView(const PassHandle pass) const
        {
            CHECK(mState == State::Execute);
            auto& tpass = mSetup->trackedPasses[pass];
            CHECK_MSG(tpass.writeBackbuffer, "Pass {} does not write to backbuffer", tpass.name);
            return mSwaps[GetSwap()].rtv.Get();
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
        void CmdDispatch(PassHandle pass, RHICommandList* cmd, RHIExtent3D thread_size) const;
        /**
         * @brief Helper that sets the current pass's PSO and descriptor sets
         * to the current command list.
         */
        void CmdSetPipeline(PassHandle pass, RHICommandList* cmd) const;
        /**
         * @brief Helper that binds a single descriptor set to the current command list.
         *
         * You generally want to use @ref CmdSetPipeline() instead.
         */
        void CmdBindDescriptorSet(PassHandle pass, RHICommandList* cmd, uint32_t index,
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
                              Optional<RHIClearColor> const& clear_rtv = RHIClearColor{},
                              Optional<RHIClearDepthStencil> const& clear_dsv = RHIClearDepthStencil{1.0f, 0u});
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
         * This value is monotonically increasing every time @ref EndExecute() is called,
         * and starts from 0.
         */
        [[nodiscard]] uint64_t GetFrame() const { return mFrame; }
        /**
         * @brief Retrieves the current swap index at the time of @ref ExecuteFrame().
         *
         * This value is associated with the current frame in flight.
         * It's guaranteed to be less than @ref GetFrameSwaps(), and starts from 0.
         *
         * This value is updated at @ref BeginExecute(), and remains
         * the same until the next @ref BeginExecute() call.
         */
        [[nodiscard]] uint32_t GetSwap() const { return mCurrentSwap; }
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
        [[nodiscard]] uint64_t GetSync() const { return mCurrentSync; }
        /**
         * @brief Returns whether async compute is enabled.
         *
         * If this returns false, all passes will be executed on the graphics queue,
         * and any queue hints passed during pass creation will be ignored.
         */
        [[nodiscard]] bool IsAsyncComputeEnabled() const { return mDesc.async; }
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
#pragma region Debugging
        [[nodiscard]] String DbgDumpGraphviz() const;
        [[nodiscard]] String DbgDumpActivePasses() const;
        [[nodiscard]] String DbgDumpExecutionGroups() const;
#pragma endregion
    };
    ENUM_NAME_CONV_BEGIN(Renderer::State)
        ENUM_NAME(Undefined);
        ENUM_NAME(Setup);
        ENUM_NAME(PostSetup);
        ENUM_NAME(Execute);
    ENUM_NAME_CONV_END();
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
    template <typename T>
    [[nodiscard]] ResourceHandle createResource(Renderer* r, StringView name, T const& desc)
    {
        return r->CreateResource(name, desc);
    }
    /**
     * @brief Convenient functional wrapper to create a sampler
     *
     * This is equivalent to calling CreateSampler(name, desc);
     */
    [[nodiscard]] inline ResourceHandle createSampler(Renderer* r, RHIDeviceSampler::SamplerDesc const& desc)
    {
        return r->CreateSampler(desc);
    }
    /**
     * @brief Convenient functional wrapper to create a pass from a RenderPass* implementation with custom priority.
     *
     * This is equivalent to calling @ref Renderer::CreatePassImpl
     *
     * @tparam T Type of @ref RenderPass to create.
     * @param queue Queue to prefer running this pass in - this is a hint, and might be ignored if async compute is
     * disabled.
     * @param priority Priority of this pass. Higher priority passes are scheduled earlier.
     */
    template <typename T, typename... Args>
        requires std::is_base_of_v<RenderPass, T>
    T* createPassImplPriority(Renderer* r, StringView name, RHIDeviceQueueType queue, size_t priority, Args&&... args)
    {
        return r->CreatePassImpl<T>(name, queue, priority, std::forward<Args>(args)...);
    }
    /**
     * @brief Convenient functional wrapper to create a pass from a RenderPass* implementation.
     *
     * This is equivalent to calling @ref createPassPriority with priority 0 for Graphics passes,
     * and priority kMaxRenderPasses for Compute passes, which schedules all Compute passes first with the best effort.
     *
     * @tparam T Type of @ref RenderPass to create.
     * @param queue Queue to prefer running this pass in - this is a hint, and might be ignored if async compute is
     * disabled.
     */
    template <typename T, typename... Args>
        requires std::is_base_of_v<RenderPass, T>
    T* createPassImpl(Renderer* r, StringView name, RHIDeviceQueueType queue, Args&&... args)
    {
        size_t pri = (queue == RHIDeviceQueueType::Graphics) ? 0 : kMaxRenderPasses;
        return createPassImpl<T>(r, name, queue, pri, std::forward<Args>(args)...);
    }
    /**
     * @brief Convenient functional wrapper to create a pass from Setup/Record lambdas with custom priority.
     *
     * This is equivalent to calling @ref Renderer::CreatePass
     *
     * @note Avoid using Lambdas with stateful captures (i.e. capturing `this` or [&]), as resource lifetimes
     *       could be _much_ involved and unpredictable. Coupled with how passes may be scheduled on different
     *       threads - refrain from shooting yourself in the foot.
     *       Prefer using stateless captures (i.e. [=]) or no captures at all, unless the states are trivial, and
     *       you really know what you're doing.
     *
     * @param queue Queue to prefer running this pass in - this is a hint, and might be ignored if async compute is
     * disabled.
     * @param priority Priority of this pass. Higher priority passes are scheduled earlier.
     * @param setup Lambda of type `void(PassHandle self, Renderer*)` called at Setup time.
     * @param record Lambda of type `void(PassHandel self, Renderer*, RHICommandList*)` called at Record time.
     * @param skip (Optional) Lambda of type `bool(PassHandle self, Renderer*)` called at Record time
     *                        to determine whether this pass should be skipped if true. This is by default always false.
     */
    template <typename FSetup, typename FRecord, typename FSkip = FSkipDefault>
    LambdaPass<FSetup, FRecord, FSkip>* createPassPriority(Renderer* r, StringView name, RHIDeviceQueueType queue,
                                                           size_t priority, FSetup&& setup, FRecord&& record,
                                                           FSkip&& skip = {})
    {
        return r->CreatePass(name, queue, priority, std::forward<FSetup>(setup), std::forward<FRecord>(record),
                             std::forward<FSkip>(skip));
    }
    /**
     * @brief Convenient functional wrapper to create a pass from Setup/Record lambdas.
     *
     * This is equivalent to calling @ref createPassPriority with priority 0 for Graphics passes,
     * and priority kMaxRenderPasses for Compute passes, which schedules all Compute passes first with the best effort.
     *
     * @note Avoid using Lambdas with stateful captures (i.e. capturing `this` or [&]), as resource lifetimes
     *       could be _much_ involved and unpredictable. Coupled with how passes may be scheduled on different
     *       threads - refrain from shooting yourself in the foot.
     *       Prefer using stateless captures (i.e. [=]) or no captures at all, unless the states are trivial, and
     *       you really know what you're doing.
     *
     * @note The priority parameter is omitted here for simplicity. If you need custom priority,
     *       use @ref createPassPriority instead.
     *
     * @param queue Queue to prefer running this pass in - this is a hint, and might be ignored if async compute is
     * disabled.
     * @param setup Lambda of type `void(PassHandle self, Renderer*)` called at Setup time.
     * @param record Lambda of type `void(PassHandel self, Renderer*, RHICommandList*)` called at Record time.
     * @param skip (Optional) Lambda of type `bool(PassHandle self, Renderer*)` called at Record time
     *                        to determine whether this pass should be skipped if true. This is by default always false.
     */
    template <typename FSetup, typename FRecord, typename FSkip = FSkipDefault>
    LambdaPass<FSetup, FRecord, FSkip>* createPass(Renderer* r, StringView name, RHIDeviceQueueType queue,
                                                   FSetup&& setup, FRecord&& record, FSkip&& skip = {})
    {
        size_t pri = (queue == RHIDeviceQueueType::Graphics) ? 0 : kMaxRenderPasses;
        return createPassPriority(r, name, queue, pri, std::forward<FSetup>(setup), std::forward<FRecord>(record),
                                  std::forward<FSkip>(skip));
    }
} // namespace Foundation::RenderCore
