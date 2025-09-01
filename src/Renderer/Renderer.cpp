#include <array>
#include <fstream>
#include <filesystem>


#include <Math/Math.hpp>
#include <Core/Allocator/StackAllocator.hpp>
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

#pragma region Render Graph Setup
void Renderer::BeginSetup() {
    m_setup = ConstructUnique<Setup>(m_allocator, m_allocator);
}
void Renderer::DeclareAccess(PassHandle pass, ResourceHandle handle, RHIResourceAccess access) {
    CHECK(m_setup && "Setup context not initialized. Did you call BeginSetup()?");
    auto& resource = m_setup->trackedResources[handle];
    if (resource.lastProducerPass.has_value()) {
        if (pass == resource.lastProducerPass.value()) {
            // No need to add self-dependency
            return;
        }
        m_setup->add_edge(pass, resource.lastProducerPass.value(), handle);
        auto& fc = m_setup->trackedPasses[resource.lastProducerPass.value()].firstConsumerPass;
        if (!fc.has_value())
            fc = pass;
    }
    using enum RHIResourceAccessBits;
    switch ((RHIResourceAccessBits)access)
    {
        case RenderTargetWrite:
        case DepthStencilWrite:
        case TransferWrite:
            resource.lastProducerPass = pass;
            break;        
    }    
    m_setup->trackedPasses[pass].resources.emplace_back(handle);
}
ResourceHandle Renderer::CreateTextureView(
    PassHandle pass, ResourceHandle handle, RHITextureViewDesc const& desc, RHITextureLayout layout, RHIResourceAccess access) {
    CHECK(m_setup && "Setup context not initialized. Did you call BeginSetup()?");
    auto& resource = m_setup->trackedResources[handle];    
    RHITextureDesc rdesc = resource.desc.visit(
        [&](RHITextureDesc const& tex) { return tex; },
        [&](RHIDeviceObjectHandle<RHITexture> const& tex) { return tex->m_desc; },
        [](auto const&) -> RHITextureDesc { throw std::runtime_error("Cannot create texture view of non-texture resource"); }
    );
    // TODO: View validation
    DeclareAccess(pass, handle, access);
    m_setup->trackedViews.emplace_back(handle, desc);
    ResourceHandle hdl = m_setup->trackedViews.size() - 1;
    m_setup->trackedPasses[pass].views.emplace_back(hdl, access, layout);
    return hdl;
}
void Renderer::CreateBufferAccess(PassHandle pass, ResourceHandle handle, RHIResourceAccess access, size_t offset, size_t size) {
    CHECK(m_setup && "Setup context not initialized. Did you call BeginSetup()?");
    auto& resource = m_setup->trackedResources[handle];
    RHIBufferDesc bdesc = resource.desc.visit(
        [&](RHIBufferDesc const& buf) { return buf; },
        [&](RHIDeviceObjectHandle<RHIBuffer> const& buf) { return buf->m_desc; },
        [](auto const&) -> RHIBufferDesc { throw std::runtime_error("Cannot create buffer access of non-buffer resource"); }
    );
    if (offset + size > bdesc.size)
        throw std::out_of_range("Buffer access out of range");    
    DeclareAccess(pass, handle, access);
    m_setup->trackedPasses[pass].bufferRanges.emplace_back(handle, access, std::pair{ offset, offset + size });
}
void Renderer::EndSetup(PassHandle EpiloguePass) {
    CHECK(m_setup && "Setup context not initialized. Did you call BeginSetup()?");
    for (PassHandle i = 0; i < m_setup->trackedPasses.size(); ++i) {
        m_setup->trackedPasses[i].pass->Setup(m_setup->trackedPasses[i].handle, *this);
    }
    StlVector<PassHandle>
        topo(m_allocator), // Topological sort
        vis(m_setup->trackedPasses.size(), m_allocator), // Visited
        depth(m_setup->trackedPasses.size(), m_allocator); // Depth in graph from EpiloguePass
    topo.reserve(m_setup->trackedPasses.size());
    auto dfs = [&](PassHandle u, PassHandle pa, auto&& dfs) -> void {
        vis[u] = 1;
        for (const auto& [v, _] : m_setup->graph[u]) {
            depth[v] = std::max(depth[u] + 1, depth[v]);
            CHECK(vis[v] != 1 && "Cyclic dependency in graph.");            
            if (vis[v] == 0) dfs(v, u, dfs);            
        }
        vis[u] = 2;
        m_setup->trackedPasses[u].used = true;
        topo.push_back(u);
    };
    dfs(EpiloguePass, -1, dfs);
    std::stable_sort(topo.begin(), topo.end(), [&](PassHandle a, PassHandle b) {
        return depth[a] > depth[b];
        // Sort by longest path.
        // This should maintain topological order (albeit in reverse)
        // and prioritize passes that are deeper in the graph        
    });
    m_setup->activePasses = topo;
    // Lifetime derived from execution order
    // Resource may be considered stale if it's not used by further passes
    // can may be released after the last use.
    for (PassHandle ord = 0; ord < m_setup->activePasses.size(); ord++) {
        auto& pass = m_setup->trackedPasses[m_setup->activePasses[ord]];
        pass.ord = ord, pass.depth = depth[pass.handle];        
        for (auto res : pass.resources) {
            if (!m_setup->activeResources.contains(res))             
                m_setup->activeResources[res] = { ord, ord };
            else {
                auto& [t_min, t_max] = m_setup->activeResources[res];
                t_min = std::min(t_min, ord);
                t_max = std::max(t_max, ord);
            }
        }
    }
    AllocateResources();    
}
void Renderer::AllocateResources() {
    CHECK(m_setup && "Setup context not initialized. Did you call BeginSetup()?");
    m_resources = ConstructUnique<Resources>(m_allocator, m_allocator);
    m_resources->fit(m_setup->trackedResources.size());
    // Create semaphores for inter-queue synchronization
    // Optionally used to be actually waited on, but will always be signaled
    for (PassHandle ord = 0; ord < m_setup->activePasses.size(); ord++) {
        auto& pass = m_setup->trackedPasses[m_setup->activePasses[ord]];
        pass.waitSemaphore = m_device->CreateSemaphore();
    }
    // TODO: Overlap transient resources with non-overlapping lifetimes
    for (auto& [handle, _] : m_setup->activeResources) {
        auto& res = m_setup->trackedResources[handle];
        res.desc.visit(
            // Owned
            [&](RHIBufferDesc const& desc)  { m_resources->resources[handle] = m_device->CreateBuffer(desc);   },
            [&](RHITextureDesc const& desc) { m_resources->resources[handle] = m_device->CreateTexture(desc); },
            // Borrowed
            [&](RHIDeviceObjectHandle<RHIBuffer> const& hdl) { m_resources->resources[handle] = hdl; },
            [&](RHIDeviceObjectHandle<RHITexture> const& hdl) { m_resources->resources[handle] = hdl; }
        );
    }
    // Create texture views
    StlVector<ResourceHandle> activeViews(m_allocator);
    for (PassHandle ord = 0; ord < m_setup->activePasses.size(); ord++) {
        auto& pass = m_setup->trackedPasses[m_setup->activePasses[ord]];
        for (auto [hdl, access, layout] : pass.views)
            activeViews.push_back(hdl);
    }
    std::sort(activeViews.begin(), activeViews.end());
    activeViews.erase(std::unique(activeViews.begin(), activeViews.end()), activeViews.end());
    m_resources->fit(activeViews.size());
    for (auto hdl : activeViews) {
        auto [rhdl, desc] = m_setup->trackedViews[hdl];
        auto& res = DereferenceResource(rhdl).Get<RHIDeviceObjectHandle<RHITexture>>();
        m_resources->views[hdl] = res->CreateTextureView(desc);
    }
}
void Renderer::PushPassBarriers(TrackedPass& pass, RHICommandList* cmd) {
    // At this point the pass execution order has been determined
    // (activePasses) and so are the resources' access patterns.
    // Minimal synchronization barriers would always be the most
    // optimal.
    // Inter-queue synchronization is handled separately at SubmitPass
    // where we wait on the producer pass's semaphore on the GPU
    cmd->BeginTransition();
    RHIPipelineStage currentStage = pass.pass->GetPipelineStage();
    // Textures
    for (auto [hdl, access, layout] : pass.views) {
        auto [rhdl, vdesc] = m_setup->trackedViews[hdl];
        auto& tres = m_setup->trackedResources[rhdl];
        auto& res = DereferenceResource(rhdl).Get<RHIDeviceObjectHandle<RHITexture>>();        
        cmd->SetImageTransition(
            res.Get(),
            {
                .src_access = tres.lastAccess,
                .dst_access = access,
                .src_stage = tres.lastStage,
                .dst_stage = currentStage,
                .src_img_layout = tres.lastLayout,
                .dst_img_layout = layout,
                .src_img_range = vdesc.range
            }
        );
        tres.lastAccess = access;
        tres.lastStage = currentStage;
        tres.lastLayout = layout;
    }
    // Buffer ranges
    for (auto [hdl, access, range] : pass.bufferRanges) {
        auto& tres = m_setup->trackedResources[hdl];
        auto& res = DereferenceResource(hdl).Get<RHIDeviceObjectHandle<RHIBuffer>>();
        cmd->SetBufferTransition(
            res.Get(),
            {
                .src_access = tres.lastAccess,
                .dst_access = access,
                .src_stage = tres.lastStage,
                .dst_stage = currentStage,
                .src_buffer_offset = range.first,
                .src_buffer_size = range.second - range.first
            }
        );
        tres.lastAccess = access;
        tres.lastStage = currentStage;
    }
    cmd->EndTransition();
}

void Renderer::SubmitPass(TrackedPass& pass, RHICommandList* cmd) {
    Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
    // Pass that depends on a producer pass that's on another queue
    // needs manual synchronization. Timeline semaphores proved to be
    // extremely useful for this - RHI only provides such abstractions.
    // Deadlocks should be impossible since we only take acyclic graphs.
    Core::StlVector<std::pair<RHIDeviceObjectHandle<RHIDeviceSemaphore>, size_t>> waits(alloc);
    auto check_wait = [&](std::optional<PassHandle> const& other) {
        if (other.has_value()) {
            auto& opass = m_setup->trackedPasses[other.value()];
            if (opass.type != pass.type) {
                CHECK(opass.waitSemaphore.IsValid() && "Pass contains invalid wait semaphore");
                // Wait on the producer pass's semaphore                
                waits.emplace_back(opass.waitSemaphore, m_frame + opass.ord);
            }
        }
    };
    // Textures
    for (auto [hdl, access, layout] : pass.views) {
        auto [rhdl, view] = m_setup->trackedViews[hdl];
        auto& res = m_setup->trackedResources[rhdl];
        check_wait(res.lastProducerPass);
    }
    // Buffer ranges
    for (auto [hdl, access, range] : pass.bufferRanges) {
        auto& res = m_setup->trackedResources[hdl];
        check_wait(res.lastProducerPass);
    }
    // Submit
    RHIDeviceQueue* queue = pass.type == PassType::Graphics ? m_gfxQueue : m_compQueue;
    queue->Submit({
        .waits = waits,
        .signals = {{{ pass.waitSemaphore, m_frame + pass.ord }}},
        .cmd_lists = { cmd },
    });
}
#pragma endregion


void Renderer::Execute(RHIExtent2D currentSize) {
    // Handle resize
    if (m_swapchain) {
        if (currentSize != m_swapchain->GetDimensions())
            CreateSwapchain(currentSize);
        uint32_t swapIndex = m_swapchain->GetNextImage(-1, {}, {});
        m_gfxQueue->Present({
            .image_index = swapIndex,
            .swapchain = m_swapchain
            });
        m_currentSwap = (m_currentSwap + 1) % m_swapchain->m_desc.buffer_count;
    }
    // Reset states
    for (auto idx : m_setup->activePasses) {
        auto& pass = m_setup->trackedPasses[idx];        
        for (auto hdl : pass.resources) {
            auto& res = m_setup->trackedResources[hdl];
            res.ResetExecuteStates();
        }
    }
    // Execute passes
    // Passes within the same depths may be recorded in parallel
    // TODO: Enable parallelism
    auto& ords = m_setup->activePasses;
    auto& passes = m_setup->trackedPasses;
    for (size_t i = 0, j = 0; i < ords.size();) {
        for (; j < ords.size() && passes[ords[j]].depth == passes[ords[i]].depth; j++) {
            auto& pass = passes[ords[j]];            
            auto cmd = m_cmdPool->CreateCommandList();
            PushPassBarriers(pass, cmd.Get());
            pass.pass->Record(pass.handle, *this, cmd.Get());
            SubmitPass(pass, cmd.Get());
        }
    }
    m_frame++;
}
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
            pass.handle,
            pass.type == PassType::Graphics ? "#d0e0f0" : "#f0d0e0");
    }
    // Dependencies
    for (PassHandle u = 0; u < m_setup->graph.size(); u++) {
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
                    cpass.name, cpass.handle,
                    pass.name, pass.handle);
        }
    }
    fmt::format_to(std::back_inserter(out), "}}\n");
    return out;
}

std::string Renderer::DbgDumpActivePasses() const {
    std::string out;
    for (const auto& idx : m_setup->activePasses) {
        auto& pass = m_setup->trackedPasses[idx];
        fmt::format_to(std::back_inserter(out), "{}: {}, dep={}, ord={}\n", pass.handle, pass.name, pass.depth, pass.ord);
    }
    return out;
}
#pragma endregion
