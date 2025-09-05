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
namespace Foundation::Rendering {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    constexpr PassHandle kRendererMaxPasses = 1024; // Maximum number of render passes
    constexpr size_t kMaxCommandListsPerSwap = 8;  // Maximum number of concurrent command buffers per swap
    const RHIPipelineStageBits kAllPipelineShaderStages =
        RHIPipelineStageBits::FragmentShader |        
        RHIPipelineStageBits::VertexShader;
    const RHIResourceAccessBits kAllShaderWrites =
        RHIResourceAccessBits::ShaderWrite |
        RHIResourceAccessBits::RenderTargetWrite |
        RHIResourceAccessBits::DepthStencilWrite |
        RHIResourceAccessBits::TransferWrite;
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
    struct TrackedPass {
        std::string name;
        PassHandle handle; // Index to tracked passes
        RHIDevicePipelineType queue; // Prefered type of queue to run in
        bool used{ false }; // Culled?
        bool has_cross_queue_dependent{ false }; // Has an edge to/from another queue
        // Writes to the swapchain backbuffer
        // Ignores other RTVs if true
        bool write_backbuffer{ false }; 
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
        TrackedPass(Allocator* alloc, PassHandle handle, std::string const& name, RHIDevicePipelineType queue, UniquePtr<RenderPass> renderPass)
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
        RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> waitSemaphore{};
        // Pipeline states for the entire pass
        RHIDeviceScopedObjectHandle<RHIPipelineState> pso;
        StlVector<RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout>> desc_layouts;
        StlVector<RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet>> desc_sets;
        StlVector<RHIDeviceDescriptorSet*> p_desc_sets;
        // ---
    };
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

        uint64_t m_frame{ 0 };

        const uint32_t m_frameSwaps{ 0 }; // Max frames in flight
        uint32_t m_currentSwap{ 0 };

        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> m_descPool;
        RHIDeviceScopedObjectHandle<RHICommandPool> m_cmdPool{};

        struct {
            RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> render{}, present{};
            RHIDeviceScopedObjectHandle<RHIDeviceFence> fence{};
            // For async compute, we might submit multiple command buffers
            // per swap. Driver usually want them to live.                       
            StlArray<RHICommandPoolScopedHandle<RHICommandList>, kMaxCommandListsPerSwap> cmds{};
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

        void ExecuteBarriers(TrackedPass& pass, RHICommandList* cmd);        
        bool ExecuteSubmitOrContinue(TrackedPass& pass, RHICommandList* cmd);
    public:
        // Enable async compute for compute passes
        bool m_enableAsyncCompute{ true };

        ~Renderer();
        Renderer(Allocator* allocator) : m_allocator(allocator), m_state(State::Undefined) {}
        Renderer(RHIApplicationObjectHandle<RHIDevice> device, RHIDeviceObjectHandle<RHISwapchain> swapchain, Allocator* allocator);

#pragma region Render Graph Setup
        /// <summary>
        /// Reset the render graph and being setup.
        /// 
        /// You must call this before Create... and Declare... calls.
        /// </summary>
        void BeginSetup();
        /// <summary>
        /// Create a render pass from a RenderPass* implementation and add it to the render graph.
        /// 
        /// This can be called inside a pass's Setup() function, or after CreatePass() but before EndSetup().
        /// </summary>
        template<typename T, typename ...Args> requires std::is_base_of_v<RenderPass, T>
        T* CreatePassImpl(std::string const& name, RHIDevicePipelineType queue, Args&&... args) {
            CHECK(m_state == State::Setup);
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
        /// <summary>
        /// Create a render pass from a Setup(Renderer*, PassHandle) and Record(Renderer*, PassHandle, RHICommandList*) lambda.
        ///
        /// NOTE: Prefer using this over CreatePass<T>() for stateless passes
        /// 
        /// This can be called inside a pass's Setup() function, or after CreatePass() but before EndSetup().
        /// </summary>
        template<typename FSetup, typename FRecord>
        LambdaPass<FSetup, FRecord>* CreatePass(std::string const& name, RHIDevicePipelineType queue, FSetup&& setup, FRecord&& record) {
            return CreatePassImpl<LambdaPass<FSetup, FRecord>>(name, queue, std::forward<FSetup>(setup), std::forward<FRecord>(record));
        }
        /// <summary>
        /// Create a new resource to be used in the render graph.
        /// 
        /// This can be called inside a pass's Setup() function, or after CreatePass() but before EndSetup().       
        /// No allocation is performed until EndSetup() is called.
        ///
        /// All resources created by a pass that is not culled will be created, regardless of usage.
        ///
        /// Resources can be imported by passing in RHIDeviceObjectHandle<RHIBuffer> or RHIDeviceObjectHandle<RHITexture>.
        /// </summary>
        template<typename T>
        ResourceHandle CreateResource(std::string const& name, T const& desc) {
            CHECK(m_state == State::Setup);
            ResourceHandle index = m_setup->trackedResources.size();
            m_setup->trackedResources.emplace_back(index, name, desc, m_allocator);
            return m_setup->trackedResources.size() - 1;
        }
        /// <summary>
        /// Creates a sampler with the specified name and descriptor.
        ///
        /// This can be called inside a pass's Setup() function, or after CreatePass() but before EndSetup().       
        /// No allocation is performed until EndSetup() is called.
        /// </summary>
        ResourceHandle CreateSampler(std::string const& name, RHIDeviceSampler::SamplerDesc const& desc);
#pragma region Resource Binding
        /// <summary>
        /// Binds shader file path to a certain pass at a certain stage.
        ///
        /// Shaders are unique per stage, and may be omitted.
        /// </summary>
        void BindShader(
            PassHandle pass, RHIShaderStage stage,
            std::filesystem::path const& shader_path
        );        
        /// <summary>
        /// Binds a shader push constant to a constant value, which can be set at Record time.
        ///
        /// Bind points are effectively shader variable names, which will be automatically dereferenced.
        /// </summary>
        /// <returns>Opaque handle value that can be used by CmdSetPushConstant to set data at Record time.</returns>
        ResourceHandle BindPushConstant(
            PassHandle pass, RHIShaderStage stage,
            size_t offset, size_t size
        );
        /// <summary>
        /// Associates Vertex Input description with this pass.
        ///
        /// This only applies to passes on Graphics queues. And will not have
        /// any effect otherwise.
        ///
        /// You MUST bind a valid Vertex Input at creation time if Draw[Indexed]
        /// is desired.       
        /// </summary>        
        void BindVertexInput(
            PassHandle pass,
            RHIPipelineState::PipelineStateDesc::VertexInput const& info
        );
        /// <summary>
        /// Binds a uniform buffer to a specified binding point in a rendering pass.
        ///
        /// Bind points are effectively shader variable names, which will be automatically dereferenced.       
        /// </summary>       
        void BindBufferUniform(
            PassHandle pass, ResourceHandle buffer,
            std::string const& bind_point
        );
        /// <summary>
        /// Binds a storage (read-write) buffer to a specified binding point.
        ///
        /// Use this for buffers declared as 'buffer' / 'RWStructuredBuffer' / 'StorageBuffer'
        /// inside shaders. Declares ShaderRead | ShaderWrite access automatically.
        /// </summary>
        void BindBufferStorage(
            PassHandle pass, ResourceHandle buffer,
            std::string const& bind_point
        );
        /// <summary>
        /// Binds a buffer for unordered (UAV) access from shaders (read and/or write in any order).
        ///
        /// Equivalent to a storage buffer but semantically indicates random R/W patterns.
        /// Declares ShaderRead | ShaderWrite access.
        /// </summary>
        void BindBufferUnordered(
            PassHandle pass, ResourceHandle buffer,
            std::string const& bind_point
        );
        /// <summary>
        /// Declares this pass has shaders that will read from this buffer.
        /// e.g. Vertex, Index
        /// </summary>        
        void BindBufferShaderRead(PassHandle pass, ResourceHandle buffer);
        /// <summary>
        /// Declares that this pass will write to the buffer via copy
        /// </summary>
        void BindBufferCopyDst(PassHandle pass, ResourceHandle buffer);
        /// <summary>
        /// Declares that this pass will read from the buffer via copy
        /// </summary>
        void BindBufferCopySrc(PassHandle pass, ResourceHandle buffer);
        /// <summary>
        /// Binds a sampler to a specified variable name in the shader.
        /// </summary>        
        void BindTextureSampler(
            PassHandle pass, ResourceHandle sampler,
            std::string const& bind_point
        );
        /// <summary>
        /// Binds a texture as a Shader Resource View (read-only sampling / fetch).
        ///
        /// A view will be created if a subresource range (mips/layers) or format reinterpretation
        /// is specified via desc. Returns the (possibly new) texture view handle.
        /// </summary>
        ResourceHandle BindTextureSRV(
            PassHandle pass, ResourceHandle texture,
            std::string const& bind_point,
            RHITextureViewDesc const& desc = {}
        );
        /// <summary>
        /// Binds a texture for unordered (UAV) read-write access in shaders.
        ///
        /// Declares ShaderRead | ShaderWrite access and sets layout to General (or equivalent),
        /// bound as StorageImage.
        ///
        /// A view will be created when desc customizes subresources or format.
        /// </summary>
        ResourceHandle BindTextureUAV(
            PassHandle pass, ResourceHandle texture, 
            std::string const& bind_point,
            RHITextureViewDesc const& desc = {}
        );
        /// <summary>
        /// Binds a texture as a Render Target View (color attachment) for a graphics pass.
        ///
        /// The pass must execute on a graphics-capable queue. Multiple RTVs may be bound.
        /// Returns the created/assigned view handle (auto-created if needed).
        ///
        /// This can be automatically bound to the pipeline with CmdBeginGraphics(), where
        /// order of multiple render targets is the same as the insertion order of the RTVs.
        /// </summary>
        ResourceHandle BindTextureRTV(
            PassHandle pass, ResourceHandle texture,
            RHITextureViewDesc const& desc = {}
        );
        /// <summary>
        /// Binds a texture as a Depth-Stencil View for a graphics pass.
        ///
        /// Only one DSV may be active per pass. Layout transitions include depth / stencil write or read.
        /// Returns the created/assigned view handle (auto-created if needed).
        ///
        /// This can be automatically bound to the pipeline with CmdBeginGraphics().
        /// </summary>
        ResourceHandle BindTextureDSV(
            PassHandle pass, ResourceHandle texture,
            RHITextureViewDesc const& desc = {}
        );
        /// <summary>
        /// Declares that this pass will write to the current (at Record time) swapchain backbuffer.
        ///
        /// ATTENTION: This invalidates any other bound RTVs.
        /// 
        /// Backbuffer in the entirety of a graphics pass is always in RenderTarget layout,
        /// and cannot be read from, copied from/to, or used as anything but.
        ///
        /// You can retrive the current backbuffer RTV via DerefCurrentBackbufferView() at Record time.
        /// 
        /// This can be automatically bound to the pipeline with CmdBeginGraphics().             
        /// </summary>        
        void BindBackbufferRTV(PassHandle pass);
        /// <summary>
        /// Declares that this pass will write to the texture via copy / blit (transfer destination).
        ///
        /// Sets TransferWrite access over the specified subresource range (all if empty).
        /// No view is created; raw resource state tracking is updated.
        /// </summary>
        void BindTextureCopyDst(
            PassHandle pass, ResourceHandle texture,
            RHITextureSubresourceRange const& range = {}
        );
        /// <summary>
        /// Declares that this pass will read from the texture via copy / blit (transfer source).
        ///
        /// Sets TransferRead access over the specified subresource range (all if empty).
        /// No view is created; raw resource state tracking is updated.
        /// </summary>
        void BindTextureCopySrc(
            PassHandle pass, ResourceHandle texture,
            RHITextureSubresourceRange const& range = {}
        );
        /* TODO: Push Constants */
#pragma endregion
        /// <summary>
        /// Finish setting up the render graph.
        ///
        /// The **last** created pass is used as the epilogue (final) pass,
        /// and will be used to determine active passes and resource lifetimes.
        /// 
        /// You must call this before Execute().
        /// </summary>
        void EndSetup();
#pragma endregion

#pragma region Swapchain
        inline RHIExtent2D GetSwapchainExtent() const {
            CHECK(m_swapchain && "Swapchain not initialized");
            return m_swapchain->m_desc.extents;
        }
        inline RHIExtent3D GetSwapchainExtent3D() const {
            CHECK(m_swapchain && "Swapchain not initialized");
            auto xy = m_swapchain->m_desc.extents;
            return { xy.x,xy.y,1 };
        }
        inline RHITexture* GetCurrentBackbuffer() {
            CHECK(m_state == State::Execute);
            return m_swapchain->GetImages()[m_currentSwap];
        }
#pragma endregion
#pragma region Render Graph Runtime
        /// <summary>
        /// Dereference a resource handle to its underlying RHI resource.
        ///
        /// This should only be called inside a pass's Record() function, or after EndSetup().
        /// </summary>        
        inline Variant<RHIBuffer*, RHITexture*> DerefResource(ResourceHandle handle) {
            CHECK(m_resources && handle < m_resources->resources.size());
            using Tv = Variant<RHIBuffer*, RHITexture*>;
            return m_resources->resources[handle].visit([](auto& hdl) -> Tv { return hdl.Get(); });
        }
        /// <summary>
        /// Dereference a texture view handle to its underlying RHI texture view.
        ///
        /// This should only be called inside a pass's Record() function, or after EndSetup().
        /// </summary>       
        inline RHITextureView* DerefTextureView(ResourceHandle handle) {
            CHECK(m_resources && handle < m_resources->views.size());
            using Tv = RHITextureView*;
            return m_resources->views[handle].visit([](auto& hdl) -> Tv { return hdl.Get(); });
        }
        /// <summary>
        /// Dereference a sampler handle to its underlying RHI sampler.
        ///
        /// This should only be called inside a pass's Record() function, or after EndSetup().
        /// </summary>        
        inline RHIDeviceSampler* DerefSampler(ResourceHandle handle) {
            CHECK(m_setup && handle < m_setup->trackedSamplers.size());
            return m_resources->samplers[handle].Get();
        }
        /// <summary>
        /// Dereference the built pipeline state object handle associated with a given pass.
        /// </summary>        
        inline RHIPipelineState* DerefPipelineState(PassHandle pass) {
            CHECK(m_setup && pass < m_setup->trackedPasses.size());
            auto& tpass = m_setup->trackedPasses[pass];
            return tpass.pso.Get();
        }
        /// <summary>
        /// Dereference the built descriptor sets associated with a given pass
        /// </summary>        
        inline StlVector<RHIDeviceDescriptorSet*> const& DerefDescriptorSets(PassHandle pass) {
            CHECK(m_setup && pass < m_setup->trackedPasses.size());
            auto& tpass = m_setup->trackedPasses[pass];
            return tpass.p_desc_sets;
        }
        /// <summary>
        /// Returns a pointer to the current backbuffer texture view.
        ///
        /// The pass must have declared BindBackbufferRTV() during setup.
        /// </summary>        
        inline RHITextureView* DerefCurrentBackbufferView(PassHandle pass) {
            CHECK(m_state == State::Execute);
            auto& tpass = m_setup->trackedPasses[pass];            
            CHECK_MSG(tpass.write_backbuffer, "Pass {} does not write to backbuffer", tpass.name);
            return m_swaps[m_currentSwap].rtv.Get();
        }        
#pragma endregion

#pragma region Command Recording Helpers
        /// <summary>
        /// Helper that pushes correct BeginGraphics() commands with
        /// declared RTVs and DSVs to the current command list.
        /// </summary>        
        void CmdBeginGraphics(
            PassHandle pass, RHICommandList* cmd,
            RHIExtent2D const& extent,
            std::optional<RHIClearColor> clear_rtv = RHIClearColor{},
            std::optional<RHIClearDepthStencil> = RHIClearDepthStencil{}
        );
        /// <summary>
        /// Helper that sets the current pass's PSO and descriptor sets 
        /// to the current command list.
        /// </summary>        
        void CmdSetPipeline(PassHandle pass, RHICommandList* cmd);
        /// <summary>
        /// Helper that sets a Push Constant range data with previously bound Push Constant handle
        /// </summary>        
        void CmdSetPushConstant(PassHandle pass, RHICommandList* cmd, ResourceHandle push_constant, size_t size, void* data);
        template<typename T> inline void CmdSetPushConstant(PassHandle pass, RHICommandList* cmd, ResourceHandle push_constant, T const& data) {
            CmdSetPushConstant(pass, cmd, push_constant, sizeof(data), &data);
        }
#pragma endregion

#pragma region Debugging
        std::string DbgDumpGraphviz() const;
        std::string DbgDumpActivePasses() const;
#pragma endregion
        /// <summary>
        /// Retrieves the current state of the renderer.
        /// </summary>        
        State GetState() const { return m_state; }
        /// <summary>
        /// Update the swapchain to a new one.
        ///
        /// You must call this when the window is resized or the swapchain is invalidated.
        ///
        /// This call will block if pending GPU work exists.
        /// </summary>        
        void SetSwapchain(RHIDeviceObjectHandle<RHISwapchain> swapchain);
        /// <summary>
        /// Run the frame. Go!
        /// </summary>
        void Execute();
    };
    /* Functional Helpers */
    /// <summary>
    /// Convinent functional wrapper to create a resource
    ///
    /// This is equivalent to calling CreateResource(name, desc);
    /// </summary>
    template<typename T>
    ResourceHandle createResource(Renderer* r, std::string const& name, T const& desc) {
        return r->CreateResource(name, desc);
    }
    /// <summary>
    /// Convenient functional wrapper to create a sampler
    ///
    /// This is equivalent to calling CreateSampler(name, desc);
    /// </summary>
    inline ResourceHandle createSampler(Renderer* r, std::string const& name, RHIDeviceSampler::SamplerDesc const& desc) {
        return r->CreateSampler(name, desc);
    }
    /// <summary>
    /// Convenient functional wrapper to create a pass from a RenderPass* implementation.
    ///
    /// This is equivalent to calling CreatePass<T>(name, queue, args...);     
    /// </summary>    
    template<typename T, typename ...Args> requires std::is_base_of_v<RenderPass, T>
    T* createPassImpl(Renderer* r, std::string const& name, RHIDevicePipelineType queue, Args&&... args) {
        return r->CreatePassImpl<T>(name, queue, std::forward<Args>(args)...);
    }
    /// <summary>
    /// Convenient functional wrapper to create a pass from Setup/Record lambdas.
    ///
    /// This is equivalent to calling CreatePass(name, queue, setup, record);
    /// </summary>
    template<typename FSetup, typename FRecord>
    LambdaPass<FSetup, FRecord>* createPass(Renderer* r, std::string const& name, RHIDevicePipelineType queue, FSetup&& setup, FRecord&& record) {
        return r->CreatePass(name, queue, std::forward<FSetup>(setup), std::forward<FRecord>(record));
    }

    ENUM_NAME_CONV_BEGIN(Renderer::State)
        case Undefined: return "Undefined";
        case Setup: return "Setup";
        case PostSetup: return "PostSetup";
        case Execute: return "Execute";
    ENUM_NAME_CONV_END();
}
