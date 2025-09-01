#pragma once
#include <RHICore/Application.hpp>
#include <RHICore/Resource.hpp>
#include <RHICore/Device.hpp>
#include <RHICore/Descriptor.hpp>
#include "RenderPass.hpp"
#include <optional>
namespace Foundation {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    constexpr PassHandle kRendererMaxPasses = 1024; // Maximum number of render passes
    struct TrackedResource {
        // Handle of the resource in the resource definitions vector
        ResourceHandle handle;
        // Name of the resource
        std::string name;
        // The resource definition itself
        ResourceDefinition desc;
        TrackedResource(ResourceHandle handle, std::string const& name, ResourceDefinition resourceDesc)
            : handle(handle), name(name), desc(resourceDesc) {}

        /* STATES */
        // Last pass that produced this resource
        std::optional<PassHandle> lastProducerPass{};
        // Last known access and stage of this resource
        RHIResourceAccess lastAccess{};
        // Last known pipeline stage of this resource
        RHIPipelineStage lastStage{};
        // (Texture) Last known layout of this resource
        RHITextureLayout lastLayout{};
        inline void ResetExecuteStates() {
            lastAccess = {};
            lastStage = {};
            lastLayout = {};
            lastProducerPass.reset();
        }
    };
    struct TrackedPass {
        struct CreationInfo {
            std::string const& name;
            const PassType type;
        };
        // Handle of the pass in the render passes vector
        PassHandle handle;
        // Name of the pass
        std::string name;
        // The render pass itself
        UniquePtr<RenderPass> pass;
        // Type of the queue to run this pass on (Graphics or Compute)
        PassType type;
        // First pass that consumes this resource
        std::optional<PassHandle> firstConsumerPass{};
        // Is pass used
        bool used{ false };
        // Depth in the dependency graph, used for scheduling
        size_t depth{};
        // Execution order index
        size_t ord{};
        // Resource handle referenced by this pass into tracked resources
        // -> trackedResources
        StlVector<ResourceHandle> resources;
        // Texture views used by this pass
        // [view handle, access, layout] -> trackedViews
        StlVector<std::tuple<ResourceHandle, RHIResourceAccess, RHITextureLayout>> views;
        // Buffer ranges used by this pass
        // [resource handle, access, {begin, end}] -> trackedResources
        StlVector<std::tuple<ResourceHandle, RHIResourceAccess, std::pair<size_t, size_t>>> bufferRanges;
        TrackedPass(Allocator* alloc, PassHandle handle, CreationInfo const& info, UniquePtr<RenderPass> renderPass)
            : resources(alloc), views(alloc), bufferRanges(alloc), handle(handle), name(info.name), pass(std::move(renderPass)), type(info.type) {
        };
        const PassHandle GetHandle() const { return handle; }
        /* STATES */
        // Semaphore to signal on pass completion
        RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> waitSemaphore{};
    };
    class Renderer {
        Allocator* m_allocator{ nullptr };

        RHIApplicationObjectHandle<RHIDevice> m_device;
        RHIDeviceScopedObjectHandle<RHISwapchain> m_swapchain;
        RHIDeviceScopedObjectHandle<RHICommandPool> m_cmdPool;
        RHIDeviceQueue* m_gfxQueue{}, * m_compQueue{};

        uint32_t m_currentSwap{ 0 };
        uint64_t m_frame{ 0 };
        void CreateSwapchain(RHIExtent2D size);

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
            void add_edge(PassHandle u, PassHandle v, ResourceHandle hdl) {
                while (u >= graph.size()) graph.emplace_back(graph.get_allocator());
                graph[u].emplace_back(v, hdl);
            }
            Setup(Allocator* allocator) : graph(allocator), trackedPasses(allocator), trackedResources(allocator), trackedViews(allocator), activePasses(allocator), activeResources(allocator) {}
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
        void AllocateResources();
        void PushPassBarriers(TrackedPass& pass, RHICommandList* cmd);
        void SubmitPass(TrackedPass& pass, RHICommandList* cmd);
        void DeclareAccess(PassHandle pass, ResourceHandle res, RHIResourceAccess access);
    public:
        Renderer(RHIApplicationObjectHandle<RHIDevice> device, RHIExtent2D drawSize, Allocator* allocator);
        Renderer(Allocator* allocator): m_allocator(allocator) {};

        void Execute(RHIExtent2D currentSize);

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
        /// </summary>        
        template<typename T>
        ResourceHandle CreateResource(std::string const& name, T const& desc) {
            CHECK(m_setup && "Setup context not initialized. Did you call BeginSetup()?");
            ResourceHandle index = m_setup->trackedResources.size();
            m_setup->trackedResources.emplace_back(index, name, desc);
            return m_setup->trackedResources.size() - 1;
        }
        /// <summary>
        /// Create a view of an existing Texture.
        /// No allocation is performed until EndSetup() is called.        
        /// 
        /// This can be called inside a pass's Setup() function, or after CreatePass() but before EndSetup().        
        /// Resource dependencies will be implcitly created.
        /// </summary>                
        ResourceHandle CreateTextureView(PassHandle pass, ResourceHandle res, RHITextureViewDesc const& desc, RHITextureLayout layout, RHIResourceAccess access = RHIResourceAccessBits::ShaderRead);
        /// <summary>
        /// Declares that a pass will access a Buffer in a certain way.
        /// Different from CreateTextureView, this merely affects the command buffer
        /// and declares the access pattern of an existing resource.
        /// 
        /// This can be called inside a pass's Setup() function, or after CreatePass() but before EndSetup().
        /// Resource dependencies will be implcitly created.        
        /// </summary>
        void CreateBufferAccess(PassHandle pass, ResourceHandle res, RHIResourceAccess access = RHIResourceAccessBits::ShaderRead, size_t offset = 0, size_t size = kFullSize);
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

        RHIDevice* GetDevice() const { return m_device.Get(); }
        RHICommandPool* GetCommandPool() const { return m_cmdPool.Get(); }
        RHIDeviceQueue* GetGfxQueue() const { return m_gfxQueue; }
        RHIDeviceQueue* GetComputeQueue() const { return m_compQueue; }
    };
}
