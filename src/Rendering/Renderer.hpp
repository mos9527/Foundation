#pragma once
#include <ranges>
#include <filesystem>
#include <RHICore/Application.hpp>
#include <RHICore/Resource.hpp>
#include <RHICore/Device.hpp>
#include <RHICore/Descriptor.hpp>
#include <RHICore/PipelineState.hpp>
#include <RHICore/Command.hpp>
#include "RenderPass.hpp"
/**
 * @brief Everything GPU related, including the Frame Graph implementation.
 */
namespace Foundation::Rendering {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    constexpr PassHandle kRendererMaxPasses = 1024; // Maximum number of render passes
    constexpr size_t kMaxCommandListsPerSwap = 128;  // Maximum number of concurrent command buffers per swap
    const RHIPipelineStageBits kAllPipelineShaderStages =
        RHIPipelineStageBits::FragmentShader |        
        RHIPipelineStageBits::VertexShader;
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
        std::string name;        
        ResourceDefinition desc;
        /* --- states --- */
        // (Texture) Per-subresource states
        size_t textureLayers{ 0 }, textureMips{ 0 };
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
            RHITextureSubresourceRange ToRange() const {
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
        StlVector<SubresourceState> lastSubresourceStates;
        inline auto GetLastSubresourceStateOf(RHITextureSubresourceRange const& range) {
            auto [mip_begin, mip_end] = range.GetMipLevelRange();
            auto [layer_begin, layer_end] = range.GetArrayLayerRange();
            CHECK(mip_begin <= mip_end && mip_end < textureMips);
            return std::views::all(StlSpan<SubresourceState>{
                lastSubresourceStates.begin() + textureLayers * mip_begin,
                    lastSubresourceStates.end()
            }) | std::views::filter([=](SubresourceState& state) {
                    return state.mip >= mip_begin && state.mip <= mip_end && state.layer >= layer_begin && state.layer <= layer_end;
                });
        }
        inline SubresourceState& GetLastSubresourceStateOf(size_t mip, size_t layer) {
            CHECK(mip < textureMips && layer < textureLayers);
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

        TrackedResource(ResourceHandle handle, std::string const& name, ResourceDefinition resourceDesc, Allocator* alloc)
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
                [&](RHIDeviceObjectHandle<RHITexture> const& tex) { update_texture_desc(tex->m_desc); }
            );
        }

    };
    /**
     * @brief Internal tracking information for a render pass in the frame graph.
     */
    struct TrackedPass {
        std::string name;
        PassHandle handle; // Index to tracked passes
        RHIDeviceQueueType queue; // Prefered type of queue to run in
        bool used{ false }; // Culled?
        bool has_cross_queue_dependent{ false }; // Has an edge to/from another queue
        // Writes to the swapchain backbuffer
        // Ignores other RTVs if true
        bool write_backbuffer{ false };
        // Uses compute shader, or runs in a compute pipeline (not necessarily a compute queue)
        // Should be mutally exclusive with write_backbuffer and other graphics states
        bool compute_pass{ false };
        // Local size for compute shaders
        std::tuple<uint32_t, uint32_t, uint32_t> compute_local_size{};
        size_t depth{}; // Depth in RG
        size_t ord{}; // Execution order
        /* -- resources -- */
        StlVector<std::tuple<
            ResourceHandle,
            RHIResourceAccess,
            RHIPipelineStage,
            RHITextureSubresourceRange,
            RHITextureLayout
            >> textureUsages; // Referenced texture subresources      
        StlVector<std::tuple<
            ResourceHandle,
            RHIResourceAccess,
            RHIPipelineStage
            >> bufferUsages; // Referenced buffers
        // Unique referenced resources (tex/buf)             
        StlVector<ResourceHandle> resources;
        // Unique texture views
        StlVector<ResourceHandle> texviews;
        /* -- pipeline -- */
        // Shader [path, entry point, stage]
        StlVector<std::tuple<
            std::filesystem::path,
            std::string,
            RHIShaderStage
            >> shaders;
        // Bind points [view(tex) or buffer(buf), desc type, binding point]        
        StlVector<std::tuple<
            ResourceHandle,
            RHIDescriptorType,
            std::string
            >> tex_bindings, buf_bindings;
        // Samplers
        StlVector<std::pair<ResourceHandle, std::string>> samplers;
        // Push Constants by [stage, offset, size]
        StlVector<RHIPipelineState::PipelineStateDesc::PushConstant> push_constants;
        // (Graphics Only) Render Target View[s]
        StlVector<ResourceHandle> rtvs;
        // (Graphics Only) Depth Stencil View
        ResourceHandle dsv{ kInvalidHandle };
        // (Graphics Only) Vertex Input assembly
        RHI::RHIPipelineState::PipelineStateDesc::VertexInput vertex_input{};
        /* --- */
        UniquePtr<RenderPass> pass;
        TrackedPass(Allocator* alloc, PassHandle handle, std::string const& name, RHIDeviceQueueType queue, UniquePtr<RenderPass> renderPass)
            : resources(alloc), bufferUsages(alloc), textureUsages(alloc),
            shaders(alloc), texviews(alloc), buf_bindings(alloc), tex_bindings(alloc),
            handle(handle),
            name(name), queue(queue),
            rtvs(alloc),
            desc_layouts(alloc), desc_sets(alloc),
            push_constants(alloc), p_desc_sets(alloc),
            samplers(alloc),
            pass(std::move(renderPass)) {
        };
        const PassHandle GetHandle() const { return handle; }
        /* -- states -- */
        /* Only unculled passes have these initialized. */
        // Semaphore to signal on pass completion
        RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> asyncSemaphore{};
        // Pipeline states for the entire pass
        RHIDeviceScopedObjectHandle<RHIPipelineState> pso;
        StlVector<RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout>> desc_layouts;
        StlVector<RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet>> desc_sets;
        StlVector<RHIDeviceDescriptorSet*> p_desc_sets;
        // ---
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

        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> m_descPool;
        RHIDeviceScopedObjectHandle<RHICommandPool> m_cmdPool{}, m_compCmdPool{}; // Graphics, Async Compute

        struct {
        private:
            // For async compute, we might submit multiple command buffers
            // per swap. Driver usually want them to live.                       
            StlArray<RHICommandPoolScopedHandle<RHICommandList>, kMaxCommandListsPerSwap> cmds{};
            StlArray<RHICommandPoolScopedHandle<RHICommandList>, kMaxCommandListsPerSwap> comp_cmds{};
            StlArray<RHIDeviceScopedObjectHandle<RHIDeviceSemaphore>, kMaxCommandListsPerSwap> barrier_semaphores{};
        public:
            RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> render{}, present{};
            RHIDeviceScopedObjectHandle<RHIDeviceFence> fence{};
            inline RHICommandList* cmd_at(size_t i, RHICommandPool* pool) {
                if (!cmds[i].IsValid())
                    cmds[i] = pool->CreateCommandList();
                return cmds[i].Get();
            }
            inline RHICommandList* comp_cmds_at(size_t i, RHICommandPool* pool) {
                if (!comp_cmds[i].IsValid())
                    comp_cmds[i] = pool->CreateCommandList();
                return comp_cmds[i].Get();
            }
            inline RHIDeviceSemaphore* barrier_semaphore_at(size_t i, RHIDevice* device) {
                if (!barrier_semaphores[i].IsValid())
                    barrier_semaphores[i] = device->CreateSemaphore(true);
                return barrier_semaphores[i].Get();
            }
            // RTV for the backbuffer
            RHITextureScopedHandle<RHITextureView> rtv{};
        } m_swaps[4];

        RHIApplicationObjectHandle<RHIDevice> m_device{};
        RHIDeviceObjectHandle<RHISwapchain> m_swapchain{};
        RHIDeviceQueue* m_gfxQueue{}, *m_compQueue{};


        struct Setup {            
            StlVector<StlVector<std::pair<PassHandle, ResourceHandle>>> graph;
            StlVector<TrackedPass> trackedPasses;
            StlVector<TrackedResource> trackedResources;
            // [resource, view desc]
            StlVector<std::pair<ResourceHandle, RHITextureViewDesc>> trackedViews;
            StlVector<RHIDeviceSampler::SamplerDesc> trackedSamplers;
            // [resource, ord range]
            StlMap<ResourceHandle, std::pair<PassHandle, PassHandle>> activeResources;
            // Passes ordered by pass.ord
            StlVector<PassHandle> execution;            
            StlMap<RHIDescriptorType, uint32_t> binding_counts;
            PassHandle epilogue{ kInvalidHandle };
            void add_edge(PassHandle u, PassHandle v, ResourceHandle hdl) {
                while (u >= graph.size()) graph.emplace_back(graph.get_allocator());
                graph[u].emplace_back(v, hdl);
            }
            Setup(Allocator* allocator) :
                graph(allocator), trackedPasses(allocator), trackedResources(allocator),
                trackedViews(allocator), execution(allocator), activeResources(allocator),
                trackedSamplers(allocator),
                binding_counts(allocator) {}
        };
        UniquePtr<Setup> m_setup;

        struct Resources {
            StlVector<Variant<
                RHIDeviceObjectHandle<RHIBuffer>,
                RHIDeviceScopedObjectHandle<RHIBuffer>,              
                RHIDeviceObjectHandle<RHITexture>,
                RHIDeviceScopedObjectHandle<RHITexture>
                >> resources;
            StlVector<Variant<
                RHITextureScopedHandle<RHITextureView>,
                RHITextureHandle<RHITextureView>
                >> views;
            StlVector<RHIDeviceScopedObjectHandle<RHIDeviceSampler>> samplers;
            Resources(Allocator* allocator) : resources(allocator), views(allocator), samplers(allocator) {}
            void fit(ResourceHandle handle) {
                resources.resize(std::max(resources.size(), handle + 1));
                views.resize(std::max(views.size(), handle + 1));
                samplers.resize(std::max(samplers.size(), handle + 1));
            }
        };
        UniquePtr<Resources> m_resources;

        void DeclareBufferAccess(PassHandle pass, ResourceHandle buffer,
            RHIPipelineStage stage,
            RHIResourceAccess access = RHIResourceAccessBits::ShaderRead
        );
        void DeclareTextureAccess(
            PassHandle pass, ResourceHandle res,
            RHIPipelineStage stage,
            RHITextureSubresourceRange range = {},
            RHIResourceAccess access = RHIResourceAccessBits::ShaderRead,
            RHITextureLayout layout = RHITextureLayout::ShaderReadOnly
        );
        ResourceHandle CreateTextureView(
            PassHandle pass, ResourceHandle res,
            RHITextureViewDesc const& desc
        );

        void CullPasses(PassHandle epilogue);        

        void BuildPipelineState(PassHandle pass);

        void FinalizeResources();
        void FinalizePSOs();

        RHIPipelineStage ExecuteGetPassAllCurrentStages(TrackedPass& pass);
        void ExecuteBarriers(TrackedPass& pass, RHICommandList* cmd);
        bool ExecuteSubmitOrContinue(TrackedPass& pass, RHICommandList* cmd, RHIDeviceQueue* queue, StlSpan<const std::pair<RHIDeviceSemaphore*, size_t>> extra_waits = {});

        void SetFrameSyncObjects();
    public:


        ~Renderer();
        Renderer(Allocator* allocator) : m_allocator(allocator), m_state(State::Undefined) {}
        Renderer(RendererDesc const& desc, RHIApplicationObjectHandle<RHIDevice> device, RHIDeviceObjectHandle<RHISwapchain> swapchain,  Allocator* allocator);

#pragma region Render Graph Setup
        /**
         * @brief Reset the render graph and being setup.
         *
         * You must call this before Create... and Declare... calls.
         */
        void BeginSetup();
        /**
         * @brief Create a render pass from a RenderPass* implementation and add it to the render graph.
         *
         * This is only available at Setup time.
         * 
         * @ref createPassImpl() should be generally preferred over this. 
         */
        template<typename T, typename ...Args> requires std::is_base_of_v<RenderPass, T>
        T* CreatePassImpl(std::string const& name, RHIDeviceQueueType queue, Args&&... args) {
            CHECK(m_state == State::Setup);
            CHECK_MSG(queue == RHIDeviceQueueType::Graphics || queue == RHIDeviceQueueType::Compute, "Invalid queue type. Only Graphics and Compute queues are supported.");
            PassHandle handle = m_setup->trackedPasses.size();
            CHECK_MSG(handle < kRendererMaxPasses, "Too many passes ({}) - leaks might be possible", handle);
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
         */
        template<typename FSetup, typename FRecord>
        LambdaPass<FSetup, FRecord>* CreatePass(std::string const& name, RHIDeviceQueueType queue, FSetup&& setup, FRecord&& record) {
            return CreatePassImpl<LambdaPass<FSetup, FRecord>>(name, queue, std::forward<FSetup>(setup), std::forward<FRecord>(record));
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
         */
        template<typename T>
        ResourceHandle CreateResource(std::string const& name, T const& desc) {
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
        ResourceHandle CreateSampler(std::string const& name, RHIDeviceSampler::SamplerDesc const& desc);
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
            std::string const& entry_point,
            std::filesystem::path const& shader_path
        );        
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
        );
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
        );
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
            std::string const& bind_point
        );
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
            std::string const& bind_point
        );
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
            std::string const& bind_point
        );
        /**
         * @brief Declares this pass has shaders that will read from this buffer.
         * e.g. Vertex, Index
         *
         * This by itself has no effect on binding. You need to call
         * cmd->BindVertexBuffer(), cmd->BindIndexBuffer() at Record time to
         * use the buffer.
         */
        void BindBufferShaderRead(PassHandle pass, ResourceHandle buffer);
        /**
         * @brief Declares that this pass will write to the buffer via copy.
         *
         * This MUST be called before calling cmd->CopyBuffer(), etc at Record time.
         */
        void BindBufferCopyDst(PassHandle pass, ResourceHandle buffer);
        /**
         * @brief Declares that this pass will read from the buffer via copy
         *
         * This MUST be called before calling cmd->CopyBuffer(), etc at Record time.
         */
        void BindBufferCopySrc(PassHandle pass, ResourceHandle buffer);
        /**
         * @brief Binds a sampler to the shader.
         *
         * Bind points are effectively shader variable names, which will be automatically dereferenced.
         */
        void BindTextureSampler(
            PassHandle pass, ResourceHandle sampler,
            std::string const& bind_point
        );
        /**
         * @brief Binds a texture as a Shader Resource View (read-only sampling / fetch).
         *
         * Bind points are effectively shader variable names, which will be automatically dereferenced.
         * 
         * No view is created until EndSetup() is called.
         */
        ResourceHandle BindTextureSRV(
            PassHandle pass, ResourceHandle texture,
            std::string const& bind_point,
            RHITextureViewDesc const& desc = {}
        );
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
            std::string const& bind_point,
            RHITextureViewDesc const& desc = {}
        );
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
        );
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
        );
        /**
         * @brief Declares that this pass will write to the current (at Record time) swapchain backbuffer.
         *
         * ATTENTION: This invalidates any other bound RTVs.
         *
         * Backbuffer in the entirety of a graphics pass is always in RenderTarget layout,
         * and cannot be read from, copied from/to, or used as anything but.
         *
         * You can retrive the current backbuffer RTV via DerefCurrentBackbufferView() at Record time.
         *
         * This can be automatically bound to the pipeline with CmdBeginGraphics().             
         */
        void BindBackbufferRTV(PassHandle pass);
        /**
         * @brief Declares that this pass will write to the texture via copy / blit (transfer destination).
         *
         * Sets TransferWrite access over the specified subresource range (all if empty).
         * No view is created; raw resource state tracking is updated.
         */
        void BindTextureCopyDst(
            PassHandle pass, ResourceHandle texture,
            RHITextureSubresourceRange const& range = {}
        );
        /**
         * @brief Declares that this pass will read from the texture via copy / blit (transfer source).
         *
         * Sets TransferRead access over the specified subresource range (all if empty).
         * No view is created; raw resource state tracking is updated.
         */
        void BindTextureCopySrc(
            PassHandle pass, ResourceHandle texture,
            RHITextureSubresourceRange const& range = {}
        );
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
        inline RHIExtent2D GetSwapchainExtent() const {
            CHECK(m_swapchain && "Swapchain not initialized");
            return m_swapchain->m_desc.extents;
        }
        /**
        * @brief Get the current swapchain extents as a 3D extent with depth 1.
        */
        inline RHIExtent3D GetSwapchainExtent3D() const {
            CHECK(m_swapchain && "Swapchain not initialized");
            auto xy = m_swapchain->m_desc.extents;
            return { xy.x,xy.y,1 };
        }
        /**
         * @brief Get the current swapchain image format.
         *
         * This is only valid in Execute() time, e.g. within a pass's Record() function.
         */
        inline RHITexture* GetCurrentBackbuffer() {
            CHECK(m_state == State::Execute && m_swapchain);
            return m_swapchain->GetImages()[m_currentSwap];
        }
#pragma endregion
#pragma region Render Graph Runtime
        /**
         * @brief Dereference a resource handle to its underlying RHI resource.
         *
         * This should only be called inside a pass's Record() function, or after EndSetup().
         */
        inline Variant<RHIBuffer*, RHITexture*> DerefResource(ResourceHandle handle) {
            CHECK(m_resources && handle < m_resources->resources.size());
            using Tv = Variant<RHIBuffer*, RHITexture*>;
            return m_resources->resources[handle].visit([](auto& hdl) -> Tv { return hdl.Get(); });
        }
        /**
         * @brief Dereference a texture view handle to its underlying RHI texture view.
         *
         * This should only be called inside a pass's Record() function, or after EndSetup().
         */
        inline RHITextureView* DerefTextureView(ResourceHandle handle) {
            CHECK(m_resources && handle < m_resources->views.size());
            using Tv = RHITextureView*;
            return m_resources->views[handle].visit([](auto& hdl) -> Tv { return hdl.Get(); });
        }
        /**
         * @brief Dereference a sampler handle to its underlying RHI sampler.
         *
         * This should only be called inside a pass's Record() function, or after EndSetup().
         */
        inline RHIDeviceSampler* DerefSampler(ResourceHandle handle) {
            CHECK(m_setup && handle < m_setup->trackedSamplers.size());
            return m_resources->samplers[handle].Get();
        }
        /**
         * @brief Dereference the built pipeline state object handle associated with a given pass.
         */
        inline RHIPipelineState* DerefPipelineState(PassHandle pass) {
            CHECK(m_setup && pass < m_setup->trackedPasses.size());
            auto& tpass = m_setup->trackedPasses[pass];
            return tpass.pso.Get();
        }
        /**
         * @brief Dereference the built descriptor sets associated with a given pass
         */
        inline StlVector<RHIDeviceDescriptorSet*> const& DerefDescriptorSets(PassHandle pass) {
            CHECK(m_setup && pass < m_setup->trackedPasses.size());
            auto& tpass = m_setup->trackedPasses[pass];
            return tpass.p_desc_sets;
        }
        /**
         * @brief Returns a pointer to the current backbuffer texture view.
         *
         * The pass must have declared BindBackbufferRTV() during setup.
         */
        inline RHITextureView* DerefCurrentBackbufferView(PassHandle pass) {
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
        RHIExtent3D CmdGetComputeLocalSize(PassHandle pass);
        /**
         * @brief Helper that dispatches a compute shader with the specified total thread count
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
        );
        /**
         * @brief Helper that sets the current pass's PSO and descriptor sets 
         * to the current command list.
         */
        void CmdSetPipeline(PassHandle pass, RHICommandList* cmd);
        /**
         * @brief Helper that pushes correct descriptor sets and PSO to the current command list, and
         * pushes correct BeginGraphics() commands with declared RTVs and DSVs to the current command list.
         */
        void CmdBeginGraphics(
            PassHandle pass, RHICommandList* cmd,
            RHIExtent2D const& extent,
            std::optional<RHIClearColor> clear_rtv = RHIClearColor{},
            std::optional<RHIClearDepthStencil> = RHIClearDepthStencil{}
        );        
        /**
         * @brief Helper that sets a Push Constant range data with a single l-value.
         */
        template<typename T> inline void CmdSetPushConstant(PassHandle pass, RHICommandList* cmd, RHIShaderStage stage, size_t offset, T const& data) {
            CHECK(m_state == State::Execute);
            auto& tpass = m_setup->trackedPasses[pass];
            cmd->PushConstant(tpass.pso.Get(), stage, (uint32_t)offset, { reinterpret_cast<const char*>(&data), sizeof(T) });
        }
#pragma endregion

#pragma region Debugging
        std::string DbgDumpGraphviz() const;
        std::string DbgDumpActivePasses() const;
#pragma endregion
        /**
         * @brief Retrieves the current state of the renderer.
         */
        State GetState() const { return m_state; }
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
     * @brief Convinent functional wrapper to create a resource
     *
     * This is equivalent to calling CreateResource(name, desc);
     */
    template<typename T>
    ResourceHandle createResource(Renderer* r, std::string const& name, T const& desc) {
        return r->CreateResource(name, desc);
    }
    /**
     * @brief Convenient functional wrapper to create a sampler
     *
     * This is equivalent to calling CreateSampler(name, desc);
     */
    inline ResourceHandle createSampler(Renderer* r, std::string const& name, RHIDeviceSampler::SamplerDesc const& desc) {
        return r->CreateSampler(name, desc);
    }
    /**
     * @brief Convenient functional wrapper to create a pass from a RenderPass* implementation.
     *
     * This is equivalent to calling CreatePass<T>(name, queue, args...);     
     */
    template<typename T, typename ...Args> requires std::is_base_of_v<RenderPass, T>
    T* createPassImpl(Renderer* r, std::string const& name, RHIDeviceQueueType queue, Args&&... args) {
        return r->CreatePassImpl<T>(name, queue, std::forward<Args>(args)...);
    }
    /**
     * @brief Convenient functional wrapper to create a pass from Setup/Record lambdas.
     *
     * This is equivalent to calling CreatePass(name, queue, setup, record);
     */
    template<typename FSetup, typename FRecord>
    LambdaPass<FSetup, FRecord>* createPass(Renderer* r, std::string const& name, RHIDeviceQueueType queue, FSetup&& setup, FRecord&& record) {
        return r->CreatePass(name, queue, std::forward<FSetup>(setup), std::forward<FRecord>(record));
    }

    ENUM_NAME_CONV_BEGIN(Renderer::State)
        case Undefined: return "Undefined";
        case Setup: return "Setup";
        case PostSetup: return "PostSetup";
        case Execute: return "Execute";
    ENUM_NAME_CONV_END();
}
