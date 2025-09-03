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

// Semaphore counter
#define SEM_COUNTER(ord) m_frame + ord

Renderer::Renderer(RHIApplicationObjectHandle<RHIDevice> device, RHIDeviceObjectHandle<RHISwapchain> swapchain, Core::Allocator* allocator)
    : m_device(device), m_allocator(allocator), m_swapchain(swapchain), m_state(State::Undefined) {
    m_gfxQueue = m_device->GetDeviceQueue(RHIDeviceQueueType::Graphics);
    m_compQueue = m_device->GetDeviceQueue(RHIDeviceQueueType::Compute);
    m_cmdPool = m_device->CreateCommandPool(RHICommandPool::PoolDesc{
        .queue = RHIDeviceQueueType::Graphics,
        .type = RHICommandPoolType::Persistent
    });
}

#pragma region Render Graph Setup
void Renderer::BeginSetup() {
    CHECK(m_state == State::Undefined || m_state == State::PostSetup);
    m_setup = ConstructUnique<Setup>(m_allocator, m_allocator);
    m_state = State::Setup;
}
ResourceHandle Renderer::CreateTextureView(
    PassHandle pass, ResourceHandle handle, RHITextureViewDesc const& desc) {
    CHECK(m_state == State::Setup);
    auto& resource = m_setup->trackedResources[handle];
    m_setup->trackedViews.emplace_back(handle, desc);
    ResourceHandle hdl = m_setup->trackedViews.size() - 1;
    m_setup->trackedPasses[pass].views.emplace_back(hdl);
    return hdl;
}
void Renderer::DeclareBufferAccess(PassHandle pass, ResourceHandle handle, RHIPipelineStage stage, RHIResourceAccess access) {
    CHECK(m_state == State::Setup);
    auto& resource = m_setup->trackedResources[handle];
    // Check for overlap
    for (auto& [h, _, __] : m_setup->trackedPasses[pass].bufferUsages)
        if (h == handle)
            throw std::runtime_error("Overlap detected. Buffer access must be global.");            
    // Add edge
    if (resource.lastBufferState.producer != kInvalidHandle)
        m_setup->add_edge(pass, resource.lastBufferState.producer, handle);
    // Set producer
    if (access & kAllShaderWrites)
        resource.lastBufferState.producer = pass;
    m_setup->trackedPasses[pass].bufferUsages.emplace_back(handle, access, stage);
    m_setup->trackedPasses[pass].resources.emplace_back(handle);
}
void Renderer::DeclareTextureAccess(
    PassHandle pass, ResourceHandle handle, RHIPipelineStage stage, RHITextureSubresourceRange range, RHIResourceAccess access, RHITextureLayout layout) {
    CHECK(m_state == State::Setup);
    auto& resource = m_setup->trackedResources[handle];
    // Check for overlap    
    auto [mip_begin, mip_end] = range.GetMipLevelRange();
    auto [layer_begin, layer_end] = range.GetArrayLayerRange();
    for (auto& [h, _, __, r, ___] : m_setup->trackedPasses[pass].textureUsages) {
        if (h == handle) {
            auto [r_mip_begin, r_mip_end] = r.GetMipLevelRange();
            auto [r_layer_begin, r_layer_end] = r.GetArrayLayerRange();
            // Mip intersects
            if (!(mip_end < r_mip_begin || mip_begin > r_mip_end)) {
                // Layer intersects
                if (!(layer_end < r_layer_begin || layer_begin > r_layer_end))
                    throw std::runtime_error("Overlap detected. Texture access must be disjoint.");
            }
            break;
        }
    }
    // Do this for all subresources in range
    for (auto& sta : resource.GetLastSubresourceStateOf(range)) {
        // Add edge
        if (sta.producer != kInvalidHandle)
            m_setup->add_edge(pass, sta.producer, handle);
        // Set producer
        if (access & kAllShaderWrites)
            sta.producer = pass;
    }
    m_setup->trackedPasses[pass].textureUsages.emplace_back(handle, access, stage, range, layout);
    m_setup->trackedPasses[pass].resources.emplace_back(handle);
}

ResourceHandle Renderer::AccessTextureSRV(
    PassHandle pass, ResourceHandle texture,
    RHITextureViewDesc const& desc
) {
    DeclareTextureAccess(pass, texture,
        kAllShaderStages,
        desc.range,
        RHIResourceAccessBits::ShaderRead,
        RHITextureLayout::ShaderReadOnly
    );
    return CreateTextureView(pass, texture, desc);
}
ResourceHandle Renderer::AccessTextureUAV(
    PassHandle pass, ResourceHandle texture,
    RHITextureViewDesc const& desc
) {
    DeclareTextureAccess(pass, texture,
        kAllShaderStages,
        desc.range,
        RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite,
        RHITextureLayout::General
    );
    return CreateTextureView(pass, texture, desc);
}
ResourceHandle Renderer::AccessTextureRTV(
    PassHandle pass, ResourceHandle texture,
    RHITextureViewDesc const& desc
) {
    DeclareTextureAccess(pass, texture,
        RHIPipelineStageBits::ColorAttachmentOutput,
        desc.range,
        RHIResourceAccessBits::RenderTargetWrite,
        RHITextureLayout::RenderTarget
    );
    return CreateTextureView(pass, texture, desc);
}
ResourceHandle Renderer::AccessTextureDSV(
    PassHandle pass, ResourceHandle texture,
    RHITextureViewDesc const& desc
) {
    DeclareTextureAccess(pass, texture,
        RHIPipelineStageBits::EarlyFragmentTests | RHIPipelineStageBits::LateFragmentTests,
        desc.range,
        RHIResourceAccessBits::DepthStencilRead | RHIResourceAccessBits::DepthStencilWrite,
        RHITextureLayout::DepthStencil
    );
    return CreateTextureView(pass, texture, desc);
}
void Renderer::AccessTextureCopyDst(
    PassHandle pass, ResourceHandle texture,
    RHITextureSubresourceRange const& range
) {
    DeclareTextureAccess(pass, texture,
        RHIPipelineStageBits::Transfer,
        range,
        RHIResourceAccessBits::TransferWrite,
        RHITextureLayout::TransferDst
    );
}
void Renderer::AccessTextureCopySrc(
    PassHandle pass, ResourceHandle texture,
    RHITextureSubresourceRange const& range
) {
    DeclareTextureAccess(pass, texture,
        RHIPipelineStageBits::Transfer,
        range,
        RHIResourceAccessBits::TransferRead,
        RHITextureLayout::TransferSrc
    );
}
void Renderer::EndSetup(PassHandle epiloguePass) {
    CHECK(m_state == State::Setup);
    // Setup all passes
    for (PassHandle i = 0; i < m_setup->trackedPasses.size(); ++i)
        m_setup->trackedPasses[i].pass->Setup(m_setup->trackedPasses[i].handle, *this);
    // Cull and do longest paths    
    StlVector<PassHandle>
        topo(m_allocator), // Topological sort
        vis(m_setup->trackedPasses.size(), m_allocator), // Visited
        depth(m_setup->trackedPasses.size(), m_allocator); // Depth in graph from epiloguePass
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
    dfs(epiloguePass, -1, dfs);
    m_setup->activePasses = topo;
    m_setup->epiloguePass = epiloguePass;
    for (PassHandle ord = 0; ord < m_setup->activePasses.size(); ord++) {
        auto& pass = m_setup->trackedPasses[m_setup->activePasses[ord]];
        // Derive lifetimes for resources from execution order
        // AllocateResources() uses this to overlap resources.        
        pass.ord = ord, pass.depth = depth[pass.handle];
        auto& resources = pass.resources;
        // Sort then make unique
        std::sort(resources.begin(), resources.end());
        resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
        for (auto res : resources) {
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
    m_state = State::PostSetup;
}
void Renderer::AllocateResources() {
    CHECK(m_state == State::Setup);
    if (!m_device) // XXX: Testing only
        return;
    m_resources = ConstructUnique<Resources>(m_allocator, m_allocator);
    m_resources->fit(m_setup->trackedResources.size());
    // Create semaphores for inter-queue synchronization
    // Optionally used to be actually waited on (inter-queue, etc), but will always be signaled
    for (PassHandle ord = 0; ord < m_setup->activePasses.size(); ord++) {
        auto& pass = m_setup->trackedPasses[m_setup->activePasses[ord]];
        pass.waitSemaphore = m_device->CreateSemaphore();
    }
    // !! TODO: Overlap transient resources to with non-overlapping lifetimes with aliasing
    for (auto& [handle, _] : m_setup->activeResources) {
        auto& res = m_setup->trackedResources[handle];
        res.desc.visit(
            // Owned
            [&](RHIBufferDesc const& desc) { m_resources->resources[handle] = m_device->CreateBuffer(desc);   },
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
        for (auto hdl : pass.views)
            activeViews.push_back(hdl);
    }
    // Instantiate active ones only
    std::sort(activeViews.begin(), activeViews.end());
    activeViews.erase(std::unique(activeViews.begin(), activeViews.end()), activeViews.end());
    m_resources->fit(activeViews.size());
    for (auto hdl : activeViews) {
        auto [rhdl, desc] = m_setup->trackedViews[hdl];
        auto& res = DereferenceResource(rhdl).Get<RHIDeviceObjectHandle<RHITexture>>();
        m_resources->views[hdl] = res->CreateTextureView(desc);
    }
    // Reset resource states
    for (auto& res : m_setup->trackedResources) {
        res.lastBufferState.reset();
        for (auto& sta : res.lastSubresourceStates)
            sta.reset();
    }
}
void Renderer::PushPassBarriers(TrackedPass& pass, RHICommandList* cmd) {
    CHECK(m_state == State::Execute);
    // At this point the pass execution order has been determined
    // (activePasses) and so are the resources' access patterns.
    // Minimal synchronization barriers would always be the most
    // optimal.
    // Inter-queue synchronization is handled separately at SubmitPass
    // where we wait on the producer pass's semaphore on the GPU
    cmd->BeginTransition();
    // Textures
    // These are always disjoint ranges
    for (auto [hdl, access, stage, range, layout] : pass.textureUsages) {
        auto& tres = m_setup->trackedResources[hdl];
        auto& res = DereferenceResource(hdl).Get<RHIDeviceObjectHandle<RHITexture>>();
        for (auto& sta : tres.GetLastSubresourceStateOf(range)) {
            if (sta.access == access && sta.stage == stage && sta.layout == layout)
                continue;
            cmd->SetImageTransition(
                res.Get(),
                {
                    .src_access = sta.access,
                    .dst_access = access,
                    .src_stage = sta.stage,
                    .dst_stage = stage,
                    .src_img_layout = sta.layout,
                    .dst_img_layout = layout,
                    .src_img_range = sta.ToRange()
                }
            );
            sta.access = access;
            sta.stage = stage;
            sta.layout = layout;
        }        
    }
    // Buffers
    // These are always global i.e. at most one per buffer per pass.
    for (auto [hdl, access, stage] : pass.bufferUsages) {
        auto& tres = m_setup->trackedResources[hdl];
        auto& res = DereferenceResource(hdl).Get<RHIDeviceObjectHandle<RHIBuffer>>();
        cmd->SetBufferTransition(
            res.Get(),
            {
                .src_access = tres.lastBufferState.access,
                .dst_access = access,
                .src_stage = tres.lastBufferState.stage,
                .dst_stage = stage,
            }
        );
        tres.lastBufferState.access = access;
        tres.lastBufferState.stage = stage;        
    }
    cmd->EndTransition();
}

void Renderer::SubmitPass(TrackedPass& pass, RHICommandList* cmd) {
    CHECK(m_state == State::Execute);
    Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
    // Pass that depends on a producer pass that's on another queue
    // needs external synchronization. Timeline semaphores proved to be
    // extremely useful for this - RHI only provides such abstractions.
    // 
    // Deadlocks should be impossible since we only produce acyclic graphs.    
    Core::StlVector<std::pair<RHIDeviceSemaphore*, size_t>> waits(&alloc);
    auto check_wait = [&](PassHandle other) {
        if (other != kInvalidHandle) {
            auto& opass = m_setup->trackedPasses[other];
            if (opass.type != pass.type) {
                CHECK(opass.waitSemaphore.IsValid() && "Pass contains invalid wait semaphore");
                // Wait on the producer pass's semaphore                       
                waits.emplace_back(opass.waitSemaphore.Get(), SEM_COUNTER(opass.ord));
            }
        }
        };
    // Textures
    for (auto [hdl, access, stage, range, layout] : pass.textureUsages) {
        auto& res = m_setup->trackedResources[hdl];
        for (auto& sta : res.GetLastSubresourceStateOf(range)) {
            if (sta.producer == pass.handle)
                continue;
            check_wait(sta.producer);
            if (access & kAllShaderWrites)
                sta.producer = pass.handle;
        }
    }
    // Buffer ranges
    for (auto [hdl, access, stage] : pass.bufferUsages) {
        auto& res = m_setup->trackedResources[hdl];
        if (res.lastBufferState.producer == pass.handle)
            continue;
        check_wait(res.lastBufferState.producer);
        if (access & kAllShaderWrites)
            res.lastBufferState.producer = pass.handle;
    }
    // Submit
    if (m_gfxQueue != m_compQueue && m_enableAsyncCompute) {
        RHIDeviceQueue* queue = pass.type == PassType::Graphics ? m_gfxQueue : m_compQueue;
        queue->Submit({
            .waits = waits,
            .signals = {{{ pass.waitSemaphore.Get(), SEM_COUNTER(pass.ord) }}},
            .cmd_lists = { cmd },
            });
    }
    else {
        // Same queue on devices that don't have dedicated compute
        // or with async compute disabled
        m_gfxQueue->Submit({            
            .signals = {{{ pass.waitSemaphore.Get(), SEM_COUNTER(pass.ord) }}},
            .cmd_lists = { cmd },
        });
    }
}
#pragma endregion


void Renderer::Execute() {
    CHECK(m_state == State::PostSetup && "Invalid state. Did you call EndSetup()?");    
    m_state = State::Execute;    
    // Execute passes
    auto& ords = m_setup->activePasses;
    auto& passes = m_setup->trackedPasses;
    for (size_t i = 0, j = 0; i < ords.size();) {
        // These passes can be executed in parallel
        // though the benefit is questionable since recording
        // isn't at all expensive, nor there's many opportunities
        // for parallelism to apply here.
        // Regardless, this is here for future-proofing.     
        for (; j < ords.size() && passes[ords[j]].depth == passes[ords[i]].depth; j++) {
            auto& pass = passes[ords[j]];
            auto cmd = m_cmdPool->CreateCommandList();
            PushPassBarriers(pass, cmd.Get());
            pass.pass->Record(pass.handle, *this, cmd.Get());
            SubmitPass(pass, cmd.Get());
        }
        i = j;
    }
    uint32_t swapIndex = m_swapchain->GetNextImage(-1, {}, {});
    if (m_setup->epiloguePass != kInvalidHandle) {
        auto& epilogue = passes[m_setup->epiloguePass];
        RHIDeviceSemaphore* waitSem = epilogue.waitSemaphore.Get();
        m_gfxQueue->Present({
            .image_index = swapIndex,
            .swapchain = m_swapchain.Get(),
            .waits = {{{ waitSem, SEM_COUNTER(epilogue.ord) }}}
            });
    }
    else {
        m_gfxQueue->Present({
        .image_index = swapIndex,
        .swapchain = m_swapchain.Get()
        });
    }
    m_currentSwap = (m_currentSwap + 1) % m_swapchain->m_desc.buffer_count;
    m_frame++;
    m_state = State::PostSetup;
}
