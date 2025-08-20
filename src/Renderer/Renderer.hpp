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
    class Renderer {
        Allocator* m_allocator{ nullptr };

        RHIApplicationObjectHandle<RHIDevice> m_device;
        RHIDeviceScopedObjectHandle<RHISwapchain> m_swapchain;
        RHIDeviceScopedObjectHandle<RHICommandPool> m_cmdPool;
        RHIDeviceQueue* m_gfxQueue{}, * m_compQueue{};

        void CreateSwapchain(RHIExtent2D size);
        
        struct TrackedResource {
            char name[32];
            ResourceDefinition desc;
            std::optional<size_t> lastProducerPass{}; // Last pass that produced this resource
            TrackedResource(const char* resourceName, ResourceDefinition resourceDesc) 
                : desc(resourceDesc) {
                snprintf(name, sizeof(name), "%s", resourceName);
            }
        };
        struct TrackedPass {
            char name[32]; // Name of the pass
            UniquePtr<RenderPass> pass; // The render pass itself
            PassType type; // Type of the queue to run this pass on (Graphics or Compute)
            TrackedPass(const char* passName, UniquePtr<RenderPass> renderPass, PassType passType)
                : pass(std::move(renderPass)), type(passType) {
                snprintf(name, sizeof(name), "%s", passName);
            }
        };
        struct SetupContext {
            StlVector<StlVector<std::pair<size_t, ResourceHandle>>> graph; // Render graph with resources as edges
            size_t currentPass{ 0 }; // Currently visiting pass in the graph
            SetupContext(Allocator* allocator) : graph(allocator) {}
            void add_edge(size_t u, size_t v, ResourceHandle w) {
                while (u >= graph.size()) graph.emplace_back(graph.get_allocator());
                graph[u].emplace_back(v, w);
            }
        };
    public:
        UniquePtr<SetupContext> m_setupContext;
        StlVector<TrackedResource> m_resourceDefines;
        StlVector<TrackedPass> m_renderPasses;

        Renderer(RHIApplicationObjectHandle<RHIDevice> device, RHIExtent2D drawSize, Allocator* allocator);
        Renderer(Allocator* allocator): 
            m_allocator(allocator), m_renderPasses(allocator), m_resourceDefines(allocator) {};

        void Draw(RHIExtent2D currentSize);

        void BeginSetup();
        template<typename T>
        ResourceHandle CreateResource(const char* name, T const& desc) {
            m_resourceDefines.emplace_back(name, desc);
            return m_resourceDefines.size() - 1;
        }
        void DeclareAccess(ResourceHandle handle, ResourceAccess access);
        template<typename T, typename ...Args>
        auto CreateGraphicsPass(const char* name, Args&&... args) {
            m_renderPasses.emplace_back(
                name,
                ConstructUniqueBase<RenderPass, T>(m_allocator, std::forward<Args>(args)...),
                PassType::Graphics
            );
            return std::pair{ m_renderPasses.size() - 1, static_cast<T*>(m_renderPasses.back().pass.get()) };
        }
        template<typename T, typename ...Args>
        auto CreateComputePass(const char* name, Args&&... args) {
            m_renderPasses.emplace_back(
                name,
                ConstructUniqueBase<RenderPass, T>(m_allocator, std::forward<Args>(args)...),
                PassType::Compute
            );
            return std::pair{ m_renderPasses.size() - 1, static_cast<T*>(m_renderPasses.back().pass.get()) };
        }
        void EndSetup();
    };
}
