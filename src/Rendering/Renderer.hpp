#pragma once
#include <ranges>
#include <utility>
#include <filesystem>
#include <RHICore/Application.hpp>
#include <RHICore/Resource.hpp>
#include <RHICore/Device.hpp>
#include <RHICore/Descriptor.hpp>
#include <RHICore/PipelineState.hpp>
#include <RHICore/Command.hpp>
#include <Allocator/StackAllocator.hpp>
#include "RenderPass.hpp"
/**
 * @brief Everything GPU related, including the Frame Graph implementation.
 */
namespace Foundation::Rendering {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    constexpr PassHandle kRendererMaxPasses = 1024; // Maximum number of render passes
    constexpr size_t kMaxCommandListsPerSwap = 128; // Maximum number of command lists per frame
    const RHIResourceAccessBits kAllShaderWrites =
        RHIResourceAccessBits::ShaderWrite |
        RHIResourceAccessBits::RenderTargetWrite |
        RHIResourceAccessBits::DepthStencilWrite |
        RHIResourceAccessBits::TransferWrite;
        
    /**
     * @brief Internal tracking information for a resource in the frame graph.
     */
    struct TrackedResource {        
        ResourceHandle handle; // Index to tracked resources
        String name;        
        ResourceDefinition desc;
        /* --- states --- */
        // (Texture) Per-subresource states
        uint32_t textureLayers{ 0 }, textureMips{ 0 };
        struct SubresourceState {
            size_t layer{ 0 }, mip{ 0 };
            PassHandle producer{ kInvalidHandle };
            RHIResourceAccess access{};
            RHIPipelineStage stage{};
            RHITextureLayout layout{};
            void reset() {
                producer = kInvalidHandle;
                access = {};
                stage = {};
                layout = {};
            }
            [[nodiscard]] RHITextureSubresourceRange ToRange() const {
                return RHITextureSubresourceRange{
                    .layer = {
                        .mip_level = static_cast<uint32_t>(mip),
                        .base_array_layer = static_cast<uint32_t>(layer),
                        .layer_count = 1
                    },
                    .mip_count = 1
                };
            }
        };
        // [mip 0 array 0, mip 0 array 1, ..., mip 1 array 0, ...]
        Vector<SubresourceState> lastSubresourceStates;
        inline auto GetLastSubresourceStateOf(RHITextureSubresourceRange const& range) {
            auto [mip_begin, mip_end] = range.GetMipLevelRange();
            auto [layer_begin, layer_end] = range.GetArrayLayerRange();
            CHECK(mip_begin <= mip_end && mip_end < textureMips);
            return std::views::all(Span<SubresourceState>{
                lastSubresourceStates.begin() + textureLayers * mip_begin,
                    lastSubresourceStates.end()
            }) | std::views::filter([=](const SubresourceState& state) {
                    return state.mip >= mip_begin && state.mip <= mip_end && state.layer >= layer_begin && state.layer <= layer_end;
                });
        }
        inline SubresourceState& GetLastSubresourceStateOf(size_t mip, size_t layer) {
            CHECK(mip <= textureMips && layer <= textureLayers);
            return lastSubresourceStates[mip * textureLayers + layer];
        }

        // (Buffer) Last known state
        // Transitions here are always global since granularity would be too fine. And seems
        // like drivers don't really care?
        // See Also: https://www.reddit.com/r/vulkan/comments/v2mswb/global_memory_barriers_vs_bufferimage_memory/
        struct BufferState {
            PassHandle producer{ kInvalidHandle };
            RHIResourceAccess access{};
            RHIPipelineStage stage{};
            void reset() {
                producer = kInvalidHandle;
                access = {};
                stage = {};
            }
        } lastBufferState{};

        TrackedResource(const ResourceHandle handle, StringView name, const ResourceDefinition& resourceDesc, Allocator* alloc)
            : handle(handle), name(name), desc(resourceDesc), lastSubresourceStates(alloc) {
            // Resize subresource states if texture
            auto update_texture_desc = [&](RHITextureDesc const& desc) {
                lastSubresourceStates.resize(desc.array_layers * desc.mip_levels);
                textureLayers = desc.array_layers;
                textureMips = desc.mip_levels;
                for (size_t i = 0; i < lastSubresourceStates.size(); i++) {
                    auto& sta = lastSubresourceStates[i];
                    sta.mip = i / desc.array_layers, sta.layer = i % desc.array_layers;
                }
                };
            desc.visit(
                [&](RHITextureDesc const& tex) { update_texture_desc(tex); },
                [&](RHIDeviceObjectHandle<RHITexture> const& tex) { update_texture_desc(tex->m_desc); },
                [&](const RHITexture* const tex) { update_texture_desc(tex->m_desc); }
            );
        }

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
        bool has_cross_queue_dependent{ false }; // Has an edge to/from another queue
        // Writes to the swapchain backbuffer
        // Ignores other RTVs if true
        bool write_backbuffer{ false };
        // Uses compute shader, or runs in a compute pipeline (not necessarily a compute queue)
        // Should be mutually exclusive with write_backbuffer and other graphics states
        bool compute_pass{ false };
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
        RHI::RHIPipelineState::PipelineStateDesc::VertexInput vertex_input{};
        /* --- */
        UniquePtr<RenderPass> pass;
        TrackedPass(Allocator* alloc, const PassHandle handle, StringView name, RHIDeviceQueueType queue, UniquePtr<RenderPass> renderPass)
            : name(name), handle(handle), queue(queue),
            textureUsages(alloc), bufferUsages(alloc), resources(alloc), texviews(alloc),
            shaders(alloc),
            tex_bindings(alloc), buf_bindings(alloc),
            samplers(alloc),
            push_constants(alloc), rtvs(alloc),
            pass(std::move(renderPass)), desc_layouts(alloc),
            desc_sets(alloc),
            p_desc_sets(alloc) {
        };
        /* -- states -- */
        /* Only unculled passes have these initialized. */
        // Semaphore to signal on pass completion
        RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> asyncSemaphore{};
        // Pipeline states for the entire pass
        RHIDeviceScopedObjectHandle<RHIPipelineState> pso;
        Vector<RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout>> desc_layouts;
        Vector<RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet>> desc_sets;
        Vector<RHIDeviceDescriptorSet*> p_desc_sets;
        // All pipeline stages used in this pass
        RHIPipelineStage GetMaxPipelineStages() const
        {
            if (queue == RHIDeviceQueueType::Graphics)
                return RHIPipelineStageBits::AllGraphics | RHIPipelineStageBits::ComputeShader;
            else
                return RHIPipelineStageBits::ComputeShader;
        }
    };
    struct RendererDesc {
        // Enable async compute
        bool async{ true }; 
        // Present the swapchain in Execute()
        bool present{ true }; 
    };
    /**
     * @brief Renderer implementing a Frame Graph system with automatic resource tracking and synchronization.
     * 
     * For usage, see also @ref Foundation::Rendering::Application
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
        RHIDeviceScopedObjectHandle<RHICommandPool> m_cmdPool{}, m_compCmdPool{}; // Graphics, Async Compute

        struct FrameSyncObjects{
        private:
            // For async compute, we might submit multiple command buffers
            // per swap. Driver usually want them to live.                       
            Vector<RHICommandPoolScopedHandle<RHICommandList>> cmds;
            Vector<RHICommandPoolScopedHandle<RHICommandList>> comp_cmds;
            Vector<RHIDeviceScopedObjectHandle<RHIDeviceSemaphore>> barrier_semaphores;
        public:
            RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> render{}, present{};
            RHIDeviceScopedObjectHandle<RHIDeviceFence> fence{};
            RHICommandList* cmd_at(size_t i, RHICommandPool* pool) {
                if (!cmds[i].IsValid())
                    cmds[i] = pool->CreateCommandList();
                return cmds[i].Get();
            }
            RHICommandList* comp_cmds_at(size_t i, RHICommandPool* pool) {
                if (!comp_cmds[i].IsValid())
                    comp_cmds[i] = pool->CreateCommandList();
                return comp_cmds[i].Get();
            }
            RHIDeviceSemaphore* barrier_semaphore_at(size_t i, RHIDevice* device) {
                if (!barrier_semaphores[i].IsValid())
                    barrier_semaphores[i] = device->CreateSemaphore(true);
                return barrier_semaphores[i].Get();
            }
            // RTV for the backbuffer
            RHITextureScopedHandle<RHITextureView> rtv{};
            // Tracked backbuffer handle
            ResourceHandle rt_handle{ kInvalidHandle };
            FrameSyncObjects(Allocator* allocator)
                : cmds(allocator), comp_cmds(allocator), barrier_semaphores(allocator)
            {
                cmds.resize(kMaxCommandListsPerSwap), comp_cmds.resize(kMaxCommandListsPerSwap),
                    barrier_semaphores.resize(kMaxCommandListsPerSwap);
            }
        };
        Vector<FrameSyncObjects> m_swaps;

        RHIApplicationObjectHandle<RHIDevice> m_device{};
        RHIDeviceObjectHandle<RHISwapchain> m_swapchain{};
        RHIDeviceQueue* m_gfxQueue{}, *m_compQueue{};

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
            void add_edge(const PassHandle u, const PassHandle v,  const ResourceHandle hdl) {
                // ReSharper disable once CppDFALoopConditionNotUpdated
                while (u >= graph.size()) graph.emplace_back(graph.get_allocator());
                graph[u].emplace_back(v, hdl);
            }
            explicit Setup(Allocator* allocator) :
                graph(allocator), trackedPasses(allocator), trackedResources(allocator),
                trackedViews(allocator), trackedSamplers(allocator), activeResources(allocator),
                execution(allocator),
                binding_counts(allocator) {}
        };
        UniquePtr<Setup> m_setup;
        // Setup
        void DeclareBufferAccess(PassHandle pass, ResourceHandle buffer,
            RHIPipelineStage stage,
            RHIResourceAccess access = RHIResourceAccessBits::ShaderRead
        ) const;
        void DeclareTextureAccess(
            PassHandle pass, ResourceHandle res,
            RHIPipelineStage stage,
            RHITextureSubresourceRange range = {},
            RHIResourceAccess access = RHIResourceAccessBits::ShaderRead,
            RHITextureLayout layout = RHITextureLayout::ShaderReadOnly
        ) const;
        [[nodiscard]] ResourceHandle CreateTextureView(
            PassHandle pass, ResourceHandle res,
            RHITextureViewDesc const& desc
        ) const;
        // PostSetup
        void CullPasses(PassHandle epilogue) const;
        void BuildPipelineState(PassHandle pass);
        void FinalizeResources();
        void FinalizePSOs();
        // Execute
        ScopedArena m_executeArena;
        // Temporary allocator for execution
        // This is reset every frame, and only guaranteed to be valid during Execute state.
        StackAllocatorSingleThreaded m_executeAlloc;
        RHIPipelineStage ExecuteGetPassAllCurrentStages(TrackedPass& pass);
        void ExecuteBarrierSubresource(TrackedResource& res, RHITextureSubresourceRange const& range, RHIResourceAccess access, RHIPipelineStage stage, RHITextureLayout layout, RHICommandList* cmd);
        void ExecuteBarrierBuffer(TrackedResource& res, RHIResourceAccess access, RHIPipelineStage stage, RHICommandList* cmd);
        void ExecuteBarriers(TrackedPass& pass, RHICommandList* cmd);
        bool ExecuteSubmitOrContinue(
            TrackedPass& pass, RHICommandList* cmd,
            const RHIDeviceQueue* queue,
            bool final_submit = false,
            Span<const Tuple<RHIDeviceSemaphore*, RHIPipelineStage, size_t>> extra_waits = {}
        );
        void SetFrameSyncObjects();

        RHIDeviceIdleGuard m_waitIdle; // Ensure device is idle on destruction
    public:
        Renderer(RendererDesc const& desc, RHIApplicationObjectHandle<RHIDevice> device, RHIDeviceObjectHandle<RHISwapchain> swapchain,  Allocator* allocator);

#pragma region Render Graph Setup
        /**
         * @brief Begins the setup phase of the render graph.
         *
         * ATTENTION: This is called at construction time, you shouldn't call this again.
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
            CHECK_MSG(handle < kRendererMaxPasses, "Too many passes ({}) - leaks might be possible", handle);
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
            std::filesystem::path const& shader_path
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
            RHITextureViewDesc const& desc = {}
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
            RHITextureViewDesc const& desc = {}
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
            RHITextureViewDesc const& desc = {}
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
            RHITextureViewDesc const& desc = {}
        ) const;
        /**
         * @brief Declares that this pass will write to the current (at Record time) swapchain backbuffer.
         *
         * ATTENTION: This invalidates any other bound RTVs. With this enabled,
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
        [[nodiscard]] inline RHIExtent2D GetSwapchainExtent() const {
            CHECK(m_swapchain && "Swapchain not initialized");
            return m_swapchain->m_desc.extents;
        }
        /**
        * @brief Get the current swapchain extents as a 3D extent with depth 1.
        */
        [[nodiscard]] inline RHIExtent3D GetSwapchainExtent3D() const {
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
         */
        [[nodiscard]] inline Variant<RHIBuffer*, RHITexture*> DerefResource(const ResourceHandle handle) const
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
        [[nodiscard]] inline RHITextureView* DerefTextureView(const ResourceHandle handle) const
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
        [[nodiscard]] inline RHIDeviceSampler* DerefSampler(const ResourceHandle handle) const
        {
            CHECK(m_setup && handle < m_setup->trackedSamplers.size());
            return m_resources->samplers[handle].Get();
        }
        /**
         * @brief Dereference the automatically built pipeline state object handle associated with a given pass.
         */
        [[nodiscard]] inline RHIPipelineState* DerefPipelineState(const PassHandle pass) const
        {
            CHECK(m_setup && pass < m_setup->trackedPasses.size());
            auto& tpass = m_setup->trackedPasses[pass];
            return tpass.pso.Get();
        }
        /**
         * @brief Dereference the built descriptor sets associated with a given pass
         */
        [[nodiscard]] inline Vector<RHIDeviceDescriptorSet*> const& DerefDescriptorSets(const PassHandle pass) const
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
        [[nodiscard]] inline RHITextureView* DerefCurrentBackbufferView(const PassHandle pass) const
        {
            CHECK(m_state == State::Execute);
            auto& tpass = m_setup->trackedPasses[pass];            
            CHECK_MSG(tpass.write_backbuffer, "Pass {} does not write to backbuffer", tpass.name);
            return m_swaps[m_currentSwap].rtv.Get();
        }        
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
         */
        void CmdBeginGraphics(
            PassHandle pass, RHICommandList* cmd,
            RHIExtent2D const& extent,
            Optional<RHIClearColor>  const& clear_rtv = RHIClearColor{},
            Optional<RHIClearDepthStencil>  const&  clear_dsv = RHIClearDepthStencil{}
        );        
        /**
         * @brief Helper that sets a Push Constant range data with a single l-value.
         */
        template<typename T> inline void CmdSetPushConstant(PassHandle pass, RHICommandList* cmd, RHIShaderStage stage, size_t offset, T const& data) {
            CHECK(m_state == State::Execute);
            auto& tpass = m_setup->trackedPasses[pass];
            cmd->PushConstant(tpass.pso.Get(), stage, static_cast<uint32_t>(offset), { reinterpret_cast<const char*>(&data), sizeof(T) });
        }
#pragma endregion

#pragma region Debugging
        [[nodiscard]] String DbgDumpGraphviz() const;
        [[nodiscard]] String DbgDumpActivePasses() const;
#pragma endregion
        /**
         * @brief Retrieves the current state of the renderer.
         */
        [[nodiscard]] State GetState() const { return m_state; }
        /**
         * @brief Update the swapchain to a new one.
         *
         * You must call this when the window is resized or the swapchain is invalidated.
         *
         * This call will block if pending GPU work exists.
         */
        void SetSwapchain(RHIDeviceObjectHandle<RHISwapchain> swapchain);
        /**
         * @brief Run the frame. Go!
         */
        void Execute();
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
