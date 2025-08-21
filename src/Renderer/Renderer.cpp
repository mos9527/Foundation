#include <array>
#include <fstream>
#include <filesystem>


#include <Math/Math.hpp>
#include <Core/Platform/Logging.hpp>
#include <RHICore/Device.hpp>

#include "Renderer.hpp"

#include <Cooking/Image.hpp>
#include <Cooking/Mesh.hpp>

#include <algorithm>
using namespace Foundation;
using namespace Foundation::Core;

void Renderer::CreateSwapchain(RHIExtent2D size) {
    m_gfxQueue->WaitIdle();
    if (m_swapchain)
        m_swapchain.Reset();
    m_swapchain = m_device->CreateSwapchain(RHISwapchain::SwapchainDesc{
        .format = RHIResourceFormat::R8G8B8A8_UNORM,
        .dimensions = size,
        .buffer_count = 3,
        .present_mode = RHISwapchain::SwapchainDesc::PresentMode::MAILBOX,
    });
}
Renderer::Renderer(RHIApplicationObjectHandle<RHIDevice> device, RHIExtent2D initialSize, Core::Allocator* allocator)
    : m_device(device), m_allocator(allocator) {
    m_gfxQueue = m_device->GetDeviceQueue(RHIDeviceQueueType::Graphics);
    m_compQueue = m_device->GetDeviceQueue(RHIDeviceQueueType::Compute);
    m_cmdPool = m_device->CreateCommandPool(RHICommandPool::PoolDesc{
        .queue = RHIDeviceQueueType::Graphics,
        .type = RHICommandPoolType::Persistent
    });
    CreateSwapchain(initialSize);
}

void Renderer::Draw(RHIExtent2D currentSize) {
    // Handle resize
    if (currentSize != m_swapchain->GetDimensions())    
        CreateSwapchain(currentSize);   
}

#pragma region Render Graph Setup
void Renderer::BeginSetup() {
    m_setup = ConstructUnique<Setup>(m_allocator, m_allocator);
}
void Renderer::DeclareAccess(ResourceHandle handle, ResourceAccess access) {
    CHECK(m_setup && "Setup context not initialized. Did you call EndSetup()?");
    auto& resource = m_setup->trackedResources[handle];
    if (resource.lastProducerPass.has_value()) {
        if (m_setup->currentPass == resource.lastProducerPass.value()) {
            // No need to add self-dependency
            return;
        }
        m_setup->add_edge(m_setup->currentPass, resource.lastProducerPass.value(), handle);
        auto& fc = m_setup->trackedPasses[resource.lastProducerPass.value()].firstConsumerPass;
        if (!fc.has_value())
            fc = m_setup->currentPass;
    }
    if (access == ResourceAccess::Write || access == ResourceAccess::ReadWrite) {
        resource.lastProducerPass = m_setup->currentPass;
    }
    m_setup->trackedPasses[m_setup->currentPass].resources.push_back(handle);
}
void Renderer::EndSetup(size_t EpiloguePass) {
    CHECK(m_setup && "Setup context not initialized. Did you call EndSetup()?");
    for (size_t i = 0; i < m_setup->trackedPasses.size(); ++i) {
        m_setup->currentPass = i;
        m_setup->trackedPasses[i].pass->Setup(*this);
    }
    StlVector<size_t>
        topo(m_allocator), // Topological sort
        vis(m_setup->trackedPasses.size(), m_allocator), // Visited
        depth(m_setup->trackedPasses.size(), m_allocator); // Depths from EpiloguePass
    topo.reserve(m_setup->trackedPasses.size());
    auto dfs = [&](size_t u, size_t pa, auto&& dfs) -> void {
        vis[u] = 1;
        for (const auto& [v, _] : m_setup->graph[u]) {
            size_t w = std::max((size_t)1, m_setup->trackedPasses[v].GetPriority());
            depth[v] = std::max(depth[u] + w, depth[v]);
            CHECK(vis[v] != 1 && "Cyclic dependency in graph.");            
            if (vis[v] == 0) dfs(v, u, dfs);            
        }
        vis[u] = 2;
        m_setup->trackedPasses[u].used = true;
        topo.push_back(u);
    };
    dfs(EpiloguePass, -1, dfs);
    std::stable_sort(topo.begin(), topo.end(), [&](size_t a, size_t b) {
        return depth[a] > depth[b];
        // Sort by longest path.
        // This should maintain topological order (albeit in reverse)
        // and prioritize passes that are deeper in the graph
        // XXX: stable_sort is unnecessary, but it helps with debugging
    });
    m_setup->activePasses = topo;
    // Lifetime derived from execution order
    // Resource may be considered stale if it's not used by further passes
    // can may be released after the last use.
    for (size_t ord = 0; ord < m_setup->activePasses.size(); ord++) {
        auto& pass = m_setup->trackedPasses[m_setup->activePasses[ord]];
        for (auto& res : pass.resources) {
            if (!m_setup->activeResources.contains(res))             
                m_setup->activeResources[res] = { ord, ord };
            else {
                auto& [t_min, t_max] = m_setup->activeResources[res];
                t_min = std::min(t_min, ord);
                t_max = std::max(t_max, ord);
            }
        }
    }
}
#pragma endregion

#pragma region Debugging
std::string Renderer::DbgDumpGraphviz() const {
    std::string out;
    fmt::format_to(std::back_inserter(out), "digraph G {{\n");
    fmt::format_to(std::back_inserter(out), "    rankdir=TB;\n");
    auto& graph = m_setup->graph;
    auto& passes = m_setup->trackedPasses;
    auto& resources = m_setup->trackedResources;
    for (auto& pass : passes) {
        fmt::format_to(
            std::back_inserter(out),
            "    \"{}@{}\" [ shape=box style=filled fillcolor=\"{}\" ];\n",
            pass.name,
            pass.index,
            pass.type == PassType::Graphics ? "#d0e0f0" : "#f0d0e0");
    }
    // Dependencies
    for (size_t u = 0; u < m_setup->graph.size(); u++) {
        for (auto [v, w] : graph[u]) {
            fmt::format_to(
                std::back_inserter(out),
                "    \"{}@{}\" -> \"{}@{}\" [label=\"{}\"];\n",
                passes[u].name, u,
                passes[v].name, v,
                resources[w].name);
        }
    }
    // Cross-queue sync points
    for (auto& pass : passes) {
        auto const& consumer = pass.firstConsumerPass;
        if (consumer.has_value()) {
            auto const& cpass = passes[consumer.value()];
            if (cpass.type != pass.type)
                fmt::format_to(
                    std::back_inserter(out),
                    "    \"{}@{}\" -> \"{}@{}\" [label=\"<x-queue syncs>\" style=dotted];\n",
                    cpass.name, cpass.index,
                    pass.name, pass.index);
        }
    }
    fmt::format_to(std::back_inserter(out), "}}\n");
    return out;
}

std::string Renderer::DbgDumpActivePasses() const {
    std::string out;
    for (const auto& idx : m_setup->activePasses) {
        auto& pass = m_setup->trackedPasses[idx];
        fmt::format_to(std::back_inserter(out), "{}: {}\n", pass.index, pass.name);
    }
    return out;
}
#pragma endregion
