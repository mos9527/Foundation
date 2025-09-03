#pragma once
#include <RHICore/Application.hpp>
#include <RHICore/Resource.hpp>
#include <RHICore/Device.hpp>
#include <RHICore/Descriptor.hpp>
#include "RenderPass.hpp"
#include <ranges>
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
        // Handle of the resource in the resource definitions vector
        ResourceHandle handle;
        // Name of the resource
        std::string name;
        // The resource definition itself
        ResourceDefinition desc;

        /* STATES */
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
        auto GetLastSubresourceStateOf(RHITextureSubresourceRange const& range) {
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
        SubresourceState GetLastSubresourceStateOf(size_t mip, size_t layer) {
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
                for (size_t i = 0; i < lastSubresourceStates.size(); i++){
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
        struct CreationInfo {
            std::string const& name;
            const PassType type = PassType::Graphics;
        };
        // Handle of the pass in the render passes vector
        PassHandle handle;
        // Name of the pass
        std::string name;
        // The render pass itself
        UniquePtr<RenderPass> pass;
        // Type of the queue preferred to run this pass on (Graphics or Compute)
        PassType type;
        // Is pass used
        bool used{ false };
        // Depth in the dependency graph, used for scheduling
        size_t depth{};
        // Execution order index
        size_t ord{};
        // Priority of the pass, higher means earlier execution within the same depth
        size_t pri{};
        // Unique resource handles referenced by this pass into tracked resources
        // These will be used to decide resource creation and lifetime
        // -> trackedResources
        StlVector<ResourceHandle> resources;
        // Texture views used by this pass
        // [view handle] -> trackedViews
        StlVector<ResourceHandle> views;
        // Texture ranges used by this pass
        // Sorted by EndSetup()
        // [resource handle, access, stage, range, layout] -> trackedResources
        StlVector<std::tuple<ResourceHandle, RHIResourceAccess, RHIPipelineStage, RHITextureSubresourceRange, RHITextureLayout>> textureUsages;
        // Buffer ranges used by this pass
        // Sorted by EndSetup()       
        // [resource handle, access, stage] -> trackedResources
        StlVector<std::tuple<ResourceHandle, RHIResourceAccess, RHIPipelineStage>> bufferUsages;
        TrackedPass(Allocator* alloc, PassHandle handle, CreationInfo const& info, UniquePtr<RenderPass> renderPass)
            : resources(alloc), views(alloc), bufferUsages(alloc), textureUsages(alloc), handle(handle), name(info.name), pass(std::move(renderPass)), type(info.type) {
        };
        const PassHandle GetHandle() const { return handle; }
        /* STATES */
        // Semaphore to signal on pass completion
        RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> waitSemaphore{};
    };
    class Renderer {
        Allocator* m_allocator{ nullptr };

        uint32_t m_currentSwap{ 0 };
        uint64_t m_frame{ 0 };

        RHIApplicationObjectHandle<RHIDevice> m_device;
        RHIDeviceObjectHandle<RHISwapchain> m_swapchain;

        RHIDeviceScopedObjectHandle<RHICommandPool> m_cmdPool;
        RHIDeviceQueue* m_gfxQueue{}, * m_compQueue{};

        struct Setup {
            // All allocated render passes
            StlVector<TrackedPass> trackedPasses;
            // All allocated resources
            StlVector<TrackedResource> trackedResources;
            // All texture views
            // Creation is always deferred, and never borrowed
            StlVector<std::pair<ResourceHandle, RHITextureViewDesc>> trackedViews;
            // Render graph with resources as edges
            StlVector<StlVector<std::pair<PassHandle, ResourceHandle>>> graph;
            // Actually used passes in the graph
            // Ordering is the execution order, with respect to dependency graph depth
            StlVector<PassHandle> activePasses;
            // All resources's life cycles to be used in the render graph
            // [index, {First used ord in activePasses, Last used ord in activePasses}]
            StlMap<ResourceHandle, std::pair<PassHandle, PassHandle>> activeResources;
            // The final pass of the frame
            PassHandle epiloguePass{ kInvalidHandle };
            void add_edge(PassHandle u, PassHandle v, ResourceHandle hdl) {
                while (u >= graph.size()) graph.emplace_back(graph.get_allocator());
                graph[u].emplace_back(v, hdl);
            }
            Setup(Allocator* allocator) :
                graph(allocator), trackedPasses(allocator), trackedResources(allocator),
                trackedViews(allocator), activePasses(allocator), activeResources(allocator) {}
        };
        UniquePtr<Setup> m_setup;

        enum class State {
            Undefined,
            Setup,
            PostSetup,
            Execute
        } m_state;

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
        void AllocateResources();
        void PushPassBarriers(TrackedPass& pass, RHICommandList* cmd);
        void SubmitPass(TrackedPass& pass, RHICommandList* cmd);
        // Helpers to create views and access for textures
        // Exported as helper functions to create DirectX/UE style SRV/UAV/RTV and co.
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
    public:
        // Enable async compute for compute passes
        bool m_enableAsyncCompute{ true };

        Renderer(Allocator* allocator) : m_allocator(allocator), m_state(State::Undefined) {}
        Renderer(RHIApplicationObjectHandle<RHIDevice> device, RHIDeviceObjectHandle<RHISwapchain> swapchain, Allocator* allocator);

        void Execute();

#pragma region Render Graph Setup
        /// <summary>
        /// Reset the render graph and being setup.
        /// 
        /// You must call this before Create... and Declare... calls.
        /// </summary>
        void BeginSetup();
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
        /// <summary>
        /// Declares that a pass will access a Buffer in a certain way.
        /// This will be used to automatically place barriers in-between passes.
        ///
        /// Access to *buffer* is unique per pass. Attempting to declare access to the same buffer
        /// multiple times in the same pass will result in an exception.
        /// 
        /// This can be called inside a pass's Setup() function, or after CreatePass() but before EndSetup().
        ///
        /// It's undefined behavior to derefence and use a resource without declaring correct access first.
        /// 
        /// Resource dependencies will be implcitly created.        
        /// </summary>
        inline void AccessBuffer(PassHandle pass, ResourceHandle buffer,
            RHIPipelineStage stage,
            RHIResourceAccess access = RHIResourceAccessBits::ShaderRead
        ) {
            DeclareBufferAccess(pass, buffer, stage, access);
        }
        /// <summary>
        /// Declares Shader Resource View (SRV/Read-Only) access for reading a texture in shaders.
        ///
        /// Access to *subresource* is unique per pass. Attempting to declare access to the same subresource
        /// multiple times in the same pass will result in an exception.
        /// </summary>
        ResourceHandle AccessTextureSRV(
            PassHandle pass, ResourceHandle texture,
            RHITextureViewDesc const& desc = {}
        );
        /// <summary>
        /// Declares Unordered Access View (UAV/Read-write) access for reading a texture in shaders.
        ///
        /// Access to *subresource* is unique per pass. Attempting to declare access to the same subresource
        /// multiple times in the same pass will result in an exception.
        /// </summary>        
        ResourceHandle AccessTextureUAV(
            PassHandle pass, ResourceHandle texture,
            RHITextureViewDesc const& desc = {}
        );
        /// <summary>
        /// Declares Render Target View (RTV/Write) access for reading a texture in shaders.
        ///
        /// Access to *subresource* is unique per pass. Attempting to declare access to the same subresource
        /// multiple times in the same pass will result in an exception.
        /// </summary>
        ResourceHandle AccessTextureRTV(
            PassHandle pass, ResourceHandle texture,
            RHITextureViewDesc const& desc = {}
        );
        /// <summary>
        /// Declares Depth-stencil view (DSV/Write, Fragment Read) access for reading a texture in shaders.
        ///
        /// Access to *subresource* is unique per pass. Attempting to declare access to the same subresource
        /// multiple times in the same pass will result in an exception.
        /// </summary>
        ResourceHandle AccessTextureDSV(
            PassHandle pass, ResourceHandle texture,
            RHITextureViewDesc const& desc = {}
        );
        /// <summary>
        /// Declares Copy Destination access for a texture.
        /// 
        /// Access to *subresource* is unique per pass. Attempting to declare access to the same subresource
        /// multiple times in the same pass will result in an exception.
        /// </summary>        
        void AccessTextureCopyDst(
            PassHandle pass, ResourceHandle texture,
            RHITextureSubresourceRange const& range = {}
        );
        /// <summary>
        /// Declares Copy Source access for a texture.
        /// 
        /// Access to *subresource* is unique per pass. Attempting to declare access to the same subresource
        /// multiple times in the same pass will result in an exception.
        /// </summary>
        void AccessTextureCopySrc(
            PassHandle pass, ResourceHandle texture,
            RHITextureSubresourceRange const& range = {}
        );
        /// <summary>
        /// Create a render pass and add it to the render graph.
        /// 
        /// This can be called inside a pass's Setup() function, or after CreatePass() but before EndSetup().
        /// </summary>        
        template<typename T, typename ...Args>
        std::pair<PassHandle, T*> CreatePass(TrackedPass::CreationInfo const& info, Args&&... args) {
            CHECK(m_setup && "Setup context not initialized. Did you call BeginSetup()?");
            PassHandle handle = m_setup->trackedPasses.size();
            CHECK(handle < kRendererMaxPasses && "Too many passes - leaks might be possible");
            m_setup->trackedPasses.emplace_back(
                m_allocator,
                handle,
                info,
                ConstructUniqueBase<RenderPass, T>(m_allocator, std::forward<Args>(args)...)
            );
            return std::pair{ handle, static_cast<T*>(m_setup->trackedPasses.back().pass.get()) };
        }
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
    };
#pragma endregion
}
