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
    constexpr size_t kRendererMaxPasses = 1024; // Maximum number of render passes
    struct TrackedResource {
        // Index of the resource in the resource definitions vector
        size_t index;
        // Name of the resource
        std::string name;
        // The resource definition itself
        ResourceDefinition desc;
        // Last pass that produced this resource
        std::optional<size_t> lastProducerPass{};
        TrackedResource(size_t index, std::string const& name, ResourceDefinition resourceDesc)
            : index(index), name(name), desc(resourceDesc) {}
    };
    struct TrackedPass {
        struct CreationInfo {
            std::string const& name;
            const PassType type;
            // Priority of the pass, used for scheduling
            // Leave this as 0 to use the default priority
            // (see GetPriority())                             
            const size_t priority = 0;
        };
        // Index of the pass in the render passes vector
        size_t index;
        // Name of the pass
        std::string name;
        // The render pass itself
        UniquePtr<RenderPass> pass;
        // Type of the queue to run this pass on (Graphics or Compute)
        PassType type;
        // First pass that consumes this resource
        std::optional<size_t> firstConsumerPass{};
        // Is pass used
        bool used{ false };
        // Priority of the pass, used for scheduling
        // The higher the eariler this pass, along with its dependents
        // are executed
        // Range: [1, inf)
        size_t priority;
        // Resources referenced by this pass
        StlVector<ResourceHandle> resources; 
        TrackedPass(Allocator* alloc, size_t index, CreationInfo const& info, UniquePtr<RenderPass> renderPass)
            : resources(alloc), index(index), name(info.name), priority(info.priority), pass(std::move(renderPass)), type(info.type) {
        };
        const size_t GetPriority() const {
            if (priority) return priority;
            // Default priority
            // Schedule compute passes ahead when possible
            return type == PassType::Compute ? kRendererMaxPasses : 1;
        }
    };
    class Renderer {
        Allocator* m_allocator{ nullptr };

        RHIApplicationObjectHandle<RHIDevice> m_device;
        RHIDeviceScopedObjectHandle<RHISwapchain> m_swapchain;
        RHIDeviceScopedObjectHandle<RHICommandPool> m_cmdPool;
        RHIDeviceQueue* m_gfxQueue{}, * m_compQueue{};

        void CreateSwapchain(RHIExtent2D size);

        struct Setup {
            // Currently visiting pass in the graph
            size_t currentPass{ 0 };
            // All allocated render passes
            StlVector<TrackedPass> trackedPasses;
            // All allocated resources
            StlVector<TrackedResource> trackedResources;
            // Render graph with resources as edges
            StlVector<StlVector<std::pair<size_t, ResourceHandle>>> graph;
            // Actually used passes in the graph
            StlVector<size_t> activePasses;
            // All resources's life cycles to be used in the render graph
            // [index, {First used ord in activePasses, Last used ord in activePasses}]
            StlMap<ResourceHandle, std::pair<size_t, size_t>> activeResources;
            void add_edge(size_t u, size_t v, ResourceHandle hdl) {
                while (u >= graph.size()) graph.emplace_back(graph.get_allocator());
                graph[u].emplace_back(v, hdl);
            }
            Setup(Allocator* allocator) : graph(allocator), trackedPasses(allocator), trackedResources(allocator), activePasses(allocator), activeResources(allocator) {}
        };
        UniquePtr<Setup> m_setup;
    public:
        Renderer(RHIApplicationObjectHandle<RHIDevice> device, RHIExtent2D drawSize, Allocator* allocator);
        Renderer(Allocator* allocator): m_allocator(allocator) {};

        void Draw(RHIExtent2D currentSize);

#pragma region Render Graph Setup
        void BeginSetup();
        template<typename T>
        ResourceHandle CreateResource(std::string const& name, T const& desc) {
            CHECK(m_setup && "Setup context not initialized. Did you call EndSetup()?");
            size_t index = m_setup->trackedResources.size();
            m_setup->trackedResources.emplace_back(index, name, desc);
            return m_setup->trackedResources.size() - 1;
        }
        void DeclareAccess(ResourceHandle handle, ResourceAccess access);
        template<typename T, typename ...Args>
        auto CreatePass(TrackedPass::CreationInfo const& info, Args&&... args) {
            CHECK(m_setup && "Setup context not initialized. Did you call EndSetup()?");
            size_t index = m_setup->trackedPasses.size();
            if (index >= kRendererMaxPasses)
                throw std::runtime_error(fmt::format("Maximum number of render passes ({}) exceeded", kRendererMaxPasses));
            m_setup->trackedPasses.emplace_back(
                m_allocator,
                index,
                info,
                ConstructUniqueBase<RenderPass, T>(m_allocator, std::forward<Args>(args)...)
            );
            return std::pair{ index, static_cast<T*>(m_setup->trackedPasses.back().pass.get()) };
        }
        void EndSetup(size_t EpiloguePass);
#pragma endregion

#pragma region Debugging
        std::string DbgDumpGraphviz() const;
        std::string DbgDumpActivePasses() const;
#pragma endregion
    };
}
