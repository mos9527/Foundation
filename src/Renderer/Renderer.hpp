#pragma once
#include <RHICore/Application.hpp>
#include <RHICore/Resource.hpp>
#include <RHICore/Device.hpp>
#include <RHICore/Descriptor.hpp>
#include <RHICore/PipelineState.hpp>
#include "RenderPass.hpp"
#include <ranges>
#include <filesystem>
namespace Foundation {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    constexpr PassHandle kRendererMaxPasses = 1024; // Maximum number of render passes
    const RHIPipelineStageBits kAllShaderStages =
        RHIPipelineStageBits::FragmentShader |
        RHIPipelineStageBits::ComputeShader |
        RHIPipelineStageBits::VertexShader |
        RHIPipelineStageBits::RayTracingShader |
        RHIPipelineStageBits::MeshShader;
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
        // ---
    };
    class Renderer {
        enum class State {
            Undefined,
            Setup,
            PostSetup,
            Execute
        } m_state;

        Allocator* m_allocator{ nullptr };

        uint32_t m_currentSwap{ 0 };
        uint64_t m_frame{ 0 };

        RHIApplicationObjectHandle<RHIDevice> m_device;
        RHIDeviceObjectHandle<RHISwapchain> m_swapchain;

        RHIDeviceScopedObjectHandle<RHICommandPool> m_cmdPool;
        RHIDeviceQueue* m_gfxQueue{}, *m_compQueue{};

        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> m_descPool;

        struct Setup {
            StlVector<StlVector<std::pair<PassHandle, ResourceHandle>>> graph;
            StlVector<TrackedPass> trackedPasses;
            StlVector<TrackedResource> trackedResources;
            StlVector<std::pair<ResourceHandle, RHITextureViewDesc>> trackedViews;
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
            Resources(Allocator* allocator) : resources(allocator), views(allocator) {}
            void fit(ResourceHandle handle) {
                resources.resize(std::max(resources.size(), handle + 1));
                views.resize(std::max(views.size(), handle + 1));
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
        void ExecuteSubmit(TrackedPass& pass, RHICommandList* cmd);
    public:
        // Enable async compute for compute passes
        bool m_enableAsyncCompute{ true };

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
        /// Create a render pass and add it to the render graph.
        /// 
        /// This can be called inside a pass's Setup() function, or after CreatePass() but before EndSetup().
        /// </summary>        
        template<typename T, typename ...Args>
        std::pair<PassHandle, T*> CreatePass(std::string const& name, RHIDevicePipelineType queue, Args&&... args) {
            CHECK(m_setup && "Setup context not initialized. Did you call BeginSetup()?");
            PassHandle handle = m_setup->trackedPasses.size();
            CHECK(handle < kRendererMaxPasses && "Too many passes - leaks might be possible");
            m_setup->trackedPasses.emplace_back(
                m_allocator,
                handle,
                name,
                queue,
                ConstructUniqueBase<RenderPass, T>(m_allocator, std::forward<Args>(args)...)
            );
            return std::pair{ handle, static_cast<T*>(m_setup->trackedPasses.back().pass.get()) };
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
            CHECK(m_setup && "Setup context not initialized. Did you call BeginSetup()?");
            ResourceHandle index = m_setup->trackedResources.size();
            m_setup->trackedResources.emplace_back(index, name, desc, m_allocator);
            return m_setup->trackedResources.size() - 1;
        }
#pragma region Resource Binding
        /// <summary>
        /// Binds shader file path to a certain pass at a certain stage.
        ///
        /// Shaders are unique per stage, and may be omitted.
        /// </summary>
        void BindShader(
            PassHandle pass, RHIShaderStage stage,
            std::filesystem::path const& shader_path, const char* entry_point
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
        /// Bin points are effectively shader variable names, which will be automatically dereferenced.       
        /// </summary>       
        void BindBufferUniform(
            PassHandle pass, ResourceHandle buffer,
            const char* bind_point
        );
        /// <summary>
        /// Binds a storage (read-write) buffer to a specified binding point.
        ///
        /// Use this for buffers declared as 'buffer' / 'RWStructuredBuffer' / 'StorageBuffer'
        /// inside shaders. Declares ShaderRead | ShaderWrite access automatically.
        /// </summary>
        void BindBufferStorage(
            PassHandle pass, ResourceHandle buffer,
            const char* bind_point
        );
        /// <summary>
        /// Binds a buffer for unordered (UAV) access from shaders (read and/or write in any order).
        ///
        /// Equivalent to a storage buffer but semantically indicates random R/W patterns.
        /// Declares ShaderRead | ShaderWrite access.
        /// </summary>
        void BindBufferUnordered(
            PassHandle pass, ResourceHandle buffer,
            const char* bind_point
        );
        /// <summary>
        /// Binds a texture as a Shader Resource View (read-only sampling / fetch).
        ///
        /// A view may be created if a subresource range (mips/layers) or format reinterpretation
        /// is specified via desc. Returns the (possibly new) texture view handle.
        /// </summary>
        ResourceHandle BindTextureSRV(
            PassHandle pass, ResourceHandle texture,
            const char* bind_point,
            RHITextureViewDesc const& desc = {}
        );
        /// <summary>
        /// Binds a texture for unordered (UAV) read-write access in shaders.
        ///
        /// Declares ShaderRead | ShaderWrite access and sets layout to General (or equivalent).
        /// A view will be created when desc customizes subresources or format.
        /// </summary>
        ResourceHandle BindTextureUAV(
            PassHandle pass, ResourceHandle texture,
            const char* bind_point,
            RHITextureViewDesc const& desc = {}
        );
        /// <summary>
        /// Binds a texture as a Render Target View (color attachment) for a graphics pass.
        ///
        /// The pass must execute on a graphics-capable queue. Multiple RTVs may be bound.
        /// Returns the created/assigned view handle (auto-created if needed).
        ///
        /// Order of multiple rendertargets are the same as the insertion order of the RTVs.
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
        /// </summary>
        ResourceHandle BindTextureDSV(
            PassHandle pass, ResourceHandle texture,
            RHITextureViewDesc const& desc = {}
        );
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
        /// You must call this before Execute().
        /// </summary>
        /// <param name="EpiloguePass">Pass where the final outputs are yielded from e.g. Tonemapping.</param>
        void EndSetup(PassHandle EpiloguePass);
#pragma endregion

#pragma region Render Graph Runtime
        /// <summary>
        /// Dereference a resource handle to its underlying RHI resource.
        ///
        /// This should only be called inside a pass's Record() function, or after EndSetup().
        /// </summary>        
        inline Variant<RHIDeviceObjectHandle<RHIBuffer>, RHIDeviceObjectHandle<RHITexture>> DereferenceResource(ResourceHandle handle) {
            CHECK(m_state == State::PostSetup || m_state == State::Execute);
            CHECK(m_resources && handle < m_resources->resources.size());
            using Tv = Variant<RHIDeviceObjectHandle<RHIBuffer>, RHIDeviceObjectHandle<RHITexture>>;
            return m_resources->resources[handle].visit(
                [](RHIDeviceObjectHandle<RHIBuffer> const& hdl) -> Tv { return hdl; },
                [](RHIDeviceScopedObjectHandle<RHIBuffer> const& hdl) -> Tv { return hdl.View(); },
                [](RHIDeviceObjectHandle<RHITexture> const& hdl) -> Tv { return hdl; },
                [](RHIDeviceScopedObjectHandle<RHITexture> const& hdl) -> Tv { return hdl.View(); }
            );
        }
        /// <summary>
        /// Dereference a texture view handle to its underlying RHI texture view.
        ///
        /// This should only be called inside a pass's Record() function, or after EndSetup().
        /// </summary>       
        inline RHITextureHandle<RHITextureView> DereferenceTextureView(ResourceHandle handle) {
            CHECK(m_state == State::PostSetup || m_state == State::Execute);
            CHECK(m_resources && handle < m_resources->views.size());
            using Tv = RHITextureHandle<RHITextureView>;
            return m_resources->views[handle].visit(
                [](RHITextureHandle<RHITextureView> const& hdl) -> Tv { return hdl; },
                [](RHITextureScopedHandle<RHITextureView> const& hdl) -> Tv { return hdl.View(); }
            );
        }
        /// <summary>
        /// Dereference the built pipeline state object handle associated with a given pass.
        /// </summary>        
        inline RHIDeviceObjectHandle<RHIPipelineState> DereferencePipelineState(PassHandle pass) {
            CHECK(m_state == State::PostSetup || m_state == State::Execute);
            CHECK(m_setup && pass < m_setup->trackedPasses.size());
            auto& tpass = m_setup->trackedPasses[pass];
            CHECK(tpass.used && "Pass is culled");
            return tpass.pso;
        }
        /// <summary>
        /// Dereference the built descriptor sets assocaited with a given pass
        /// </summary>        
        inline StlVector<RHIDeviceDescriptorPoolScopedHandle<RHIDeviceDescriptorSet>> const& DereferenceDescriptorSets(PassHandle pass) {
            CHECK(m_state == State::PostSetup || m_state == State::Execute);
            CHECK(m_setup && pass < m_setup->trackedPasses.size());
            auto& tpass = m_setup->trackedPasses[pass];
            CHECK(tpass.used && "Pass is culled");
            return tpass.desc_sets;
        }
#pragma endregion

#pragma region Debugging
        std::string DbgDumpGraphviz() const;
        std::string DbgDumpActivePasses() const;
#pragma endregion

#pragma region Device Resources
        inline RHIExtent2D GetSwapchainExtent() const {
            CHECK(m_swapchain && "Swapchain not initialized");
            return m_swapchain->m_desc.extents;
        }
        inline RHIDevice* GetDevice() const { return m_device.Get(); }
        inline RHICommandPool* GetCommandPool() const { return m_cmdPool.Get(); }
        inline RHIDeviceQueue* GetGfxQueue() const { return m_gfxQueue; }
        inline RHIDeviceQueue* GetComputeQueue() const { return m_compQueue; }
#pragma endregion
        /// <summary>
        /// Run the frame. Go!
        /// </summary>
        void Execute();
    };

}
