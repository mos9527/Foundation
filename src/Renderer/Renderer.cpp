#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>


#include <Math/Math.hpp>
#include <Core/Allocator/StackAllocator.hpp>
#include <RHICore/Device.hpp>

#include "Renderer.hpp"
#include "ShaderReflection.hpp"

using namespace Foundation;
using namespace Foundation::Core;

// Semaphore counter
#define SEM_COUNTER(ord) m_frame + ord + 1LL

Renderer::Renderer(RHIApplicationObjectHandle<RHIDevice> device, RHIDeviceObjectHandle<RHISwapchain> swapchain, Core::Allocator* allocator)
    : m_device(device), m_allocator(allocator), m_swapchain(swapchain), m_state(State::Undefined), m_frameSwaps(swapchain->GetImages().size()) {
    m_gfxQueue = m_device->GetDeviceQueue(RHIDeviceQueueType::Graphics);
    m_compQueue = m_device->GetDeviceQueue(RHIDeviceQueueType::Compute);
    m_cmdPool = m_device->CreateCommandPool(RHICommandPool::PoolDesc{
        .queue = RHIDeviceQueueType::Graphics,
        .type = RHICommandPoolType::Persistent
    });
    for (size_t i = 0; i < m_frameSwaps; i++) {
        m_swaps[i].render = m_device->CreateSemaphore(false);
        m_swaps[i].present = m_device->CreateSemaphore(false);
        m_swaps[i].fence = m_device->CreateFence();
        for (size_t j = 0; j < kMaxCommandListsPerSwap; j++)
            m_swaps[i].cmds[j] = m_cmdPool->CreateCommandList();
    }
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
    m_setup->trackedPasses[pass].texviews.emplace_back(hdl);
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
    if (resource.lastBufferState.producer != kInvalidHandle) {
        m_setup->add_edge(pass, resource.lastBufferState.producer, handle);
        if (m_setup->trackedPasses[resource.lastBufferState.producer].queue != m_setup->trackedPasses[pass].queue) {
            // Cross-queue dependency
            m_setup->trackedPasses[resource.lastBufferState.producer].has_cross_queue_dependent = true;
        }
    }
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
        if (sta.producer != kInvalidHandle) {
            m_setup->add_edge(pass, sta.producer, handle);
            if (m_setup->trackedPasses[sta.producer].queue != m_setup->trackedPasses[pass].queue) {
                // Cross-queue dependency
                m_setup->trackedPasses[sta.producer].has_cross_queue_dependent = true;
            }
        }
        // Set producer
        if (access & kAllShaderWrites)
            sta.producer = pass;
    }
    m_setup->trackedPasses[pass].textureUsages.emplace_back(handle, access, stage, range, layout);
    m_setup->trackedPasses[pass].resources.emplace_back(handle);
}

/* -- binding -- */
void Renderer::BindShader(
    PassHandle pass, RHIShaderStage stage,
    std::filesystem::path const& shader_path, const char* entry_point
) {
    CHECK(m_state == State::Setup);
    CHECK(stage.is_bitmask() && "Only one stage can be bound to a shader per pass");
    for (auto& [_, __, s] : m_setup->trackedPasses[pass].shaders)
        if (s == stage)
            throw std::runtime_error("Shader stage already bound.");
    m_setup->trackedPasses[pass].shaders.emplace_back(shader_path, entry_point, stage);
}
void Renderer::BindVertexInput(
    PassHandle pass,
    RHIPipelineState::PipelineStateDesc::VertexInput const& info
) {
    CHECK(m_state == State::Setup);
    m_setup->trackedPasses[pass].vertex_input = info;
}
ResourceHandle Renderer::BindPushConstant(
    PassHandle pass, RHIShaderStage stage,
    size_t offset, size_t size
) {
    CHECK(m_state == State::Setup);
    for (auto& [s, _, __] : m_setup->trackedPasses[pass].push_constants)
        if (s == stage)
            throw std::runtime_error("Shader stage already has Push Constants");
    m_setup->trackedPasses[pass].push_constants.emplace_back(stage, offset, size);
    return m_setup->trackedPasses[pass].push_constants.size() - 1;
}
void Renderer::BindBufferUniform(
    PassHandle pass, ResourceHandle buffer,
    const char* bind_point
) {
    CHECK(m_state == State::Setup);
    DeclareBufferAccess(pass, buffer,
        kAllPipelineShaderStages,
        RHIResourceAccessBits::UniformRead
    );
    m_setup->trackedPasses[pass].buf_bindings.emplace_back(buffer, RHIDescriptorType::UniformBuffer, bind_point);
    m_setup->binding_counts[RHIDescriptorType::UniformBuffer]++;
}
void Renderer::BindBufferStorage(
    PassHandle pass, ResourceHandle buffer,
    const char* bind_point
) {
    CHECK(m_state == State::Setup);
    DeclareBufferAccess(pass, buffer,
        kAllPipelineShaderStages,
        RHIResourceAccessBits::ShaderRead
    );
    m_setup->trackedPasses[pass].buf_bindings.emplace_back(buffer, RHIDescriptorType::StorageBuffer, bind_point);
    m_setup->binding_counts[RHIDescriptorType::StorageBuffer]++;
}
void Renderer::BindBufferUnordered(
    PassHandle pass, ResourceHandle buffer,
    const char* bind_point
) {
    CHECK(m_state == State::Setup);
    DeclareBufferAccess(pass, buffer,
        kAllPipelineShaderStages,
        RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite
    );
    m_setup->trackedPasses[pass].buf_bindings.emplace_back(buffer, RHIDescriptorType::StorageBuffer, bind_point);
    m_setup->binding_counts[RHIDescriptorType::StorageBuffer]++;
}
void Renderer::BindBufferCopyDst(PassHandle pass, ResourceHandle buffer) {
    CHECK(m_state == State::Setup);
    DeclareBufferAccess(pass, buffer,
        RHIPipelineStageBits::Transfer,
        RHIResourceAccessBits::TransferWrite
    );
}
void Renderer::BindBufferCopySrc(PassHandle pass, ResourceHandle buffer) {
    CHECK(m_state == State::Setup);
    DeclareBufferAccess(pass, buffer,
        RHIPipelineStageBits::Transfer,
        RHIResourceAccessBits::TransferRead
    );
}
ResourceHandle Renderer::BindTextureSRV(
    PassHandle pass, ResourceHandle texture,
    const char* shader_name,
    RHITextureViewDesc const& desc
) {
    CHECK(m_state == State::Setup);
    DeclareTextureAccess(pass, texture,
        kAllPipelineShaderStages,
        desc.range,
        RHIResourceAccessBits::ShaderRead,
        RHITextureLayout::ShaderReadOnly
    );
    ResourceHandle view = CreateTextureView(pass, texture, desc);
    m_setup->trackedPasses[pass].tex_bindings.emplace_back(view, RHIDescriptorType::SampledImage, shader_name);
    m_setup->binding_counts[RHIDescriptorType::SampledImage]++;
    return view;
}
ResourceHandle Renderer::BindTextureUAV(
    PassHandle pass, ResourceHandle texture,
    const char* shader_name,
    RHITextureViewDesc const& desc
) {
    CHECK(m_state == State::Setup);
    DeclareTextureAccess(pass, texture,
        kAllPipelineShaderStages,
        desc.range,
        RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite,
        RHITextureLayout::General
    );
    ResourceHandle view = CreateTextureView(pass, texture, desc);
    m_setup->trackedPasses[pass].tex_bindings.emplace_back(view, RHIDescriptorType::StorageImage, shader_name);
    m_setup->binding_counts[RHIDescriptorType::StorageImage]++;
    return view;
}
ResourceHandle Renderer::BindTextureRTV(
    PassHandle pass, ResourceHandle texture,
    RHITextureViewDesc const& desc
) {
    CHECK(m_state == State::Setup);
    auto& tpass = m_setup->trackedPasses[pass];
    CHECK(tpass.queue == RHIDevicePipelineType::Graphics && "RTV (Render Target Views) are only supported on Graphics queues");
    DeclareTextureAccess(pass, texture,
        RHIPipelineStageBits::ColorAttachmentOutput,
        desc.range,
        RHIResourceAccessBits::RenderTargetWrite,
        RHITextureLayout::RenderTarget
    );
    ResourceHandle view = CreateTextureView(pass, texture, desc);
    tpass.rtvs.push_back(view);
    return view;
}
ResourceHandle Renderer::BindTextureDSV(
    PassHandle pass, ResourceHandle texture,
    RHITextureViewDesc const& desc
) {
    CHECK(m_state == State::Setup);
    auto& tpass = m_setup->trackedPasses[pass];
    CHECK(tpass.queue == RHIDevicePipelineType::Graphics && "DSV (Depth Stencil Views) are only supported on Graphics queues");
    DeclareTextureAccess(pass, texture,
        RHIPipelineStageBits::EarlyFragmentTests | RHIPipelineStageBits::LateFragmentTests,
        desc.range,
        RHIResourceAccessBits::DepthStencilRead | RHIResourceAccessBits::DepthStencilWrite,
        RHITextureLayout::DepthStencil
    );
    ResourceHandle view = CreateTextureView(pass, texture, desc);
    tpass.dsv = view;
    return view;
}
void Renderer::BindTextureCopyDst(
    PassHandle pass, ResourceHandle texture,
    RHITextureSubresourceRange const& range
) {
    CHECK(m_state == State::Setup);
    DeclareTextureAccess(pass, texture,
        RHIPipelineStageBits::Transfer,
        range,
        RHIResourceAccessBits::TransferWrite,
        RHITextureLayout::TransferDst
    );
}
void Renderer::BindTextureCopySrc(
    PassHandle pass, ResourceHandle texture,
    RHITextureSubresourceRange const& range
) {
    CHECK(m_state == State::Setup);
    DeclareTextureAccess(pass, texture,
        RHIPipelineStageBits::Transfer,
        range,
        RHIResourceAccessBits::TransferRead,
        RHITextureLayout::TransferSrc
    );
}
/* --- */
void Renderer::EndSetup(PassHandle epilogue) {
    CHECK(m_state == State::Setup);
    // Setup all passes
    for (PassHandle i = 0; i < m_setup->trackedPasses.size(); ++i) {
        auto& pass = m_setup->trackedPasses[i];
        pass.pass->Setup(pass.handle, *this);
    }
    CullPasses(epilogue);
    FinalizeResources();
    FinalizePSOs();
    m_state = State::PostSetup;
}
void Renderer::CullPasses(PassHandle epilogue) {
    CHECK(m_state == State::Setup);
    // Cull and topsort
    StlVector<PassHandle>
        topo(m_allocator),
        vis(m_setup->trackedPasses.size(), m_allocator),
        depth(m_setup->trackedPasses.size(), m_allocator); // Depth in graph from epilogue
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
    if (m_setup->graph.size()) {
        dfs(epilogue, -1, dfs);
        m_setup->execution = topo;
    }
    else {
        // No dependency from any passes
        // Execute only the epilouge
        m_setup->execution.push_back(epilogue);
    }
    m_setup->epilogue = epilogue;
    for (PassHandle ord = 0; ord < m_setup->execution.size(); ord++) {
        auto& pass = m_setup->trackedPasses[m_setup->execution[ord]];
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
}
void Renderer::BuildPipelineState(PassHandle pass) {
    auto& tracked = m_setup->trackedPasses[pass];
    StlVector<RHIPipelineState::PipelineStateDesc::ShaderStage> pso_stages(m_allocator);
    // Load shader bytecode
    LOG_RUNTIME(Renderer, debug, "Building PSO for {} [{}]", tracked.name, pass);
    StlVector<char> data(m_allocator);
    StlVector<RHIDeviceScopedObjectHandle<RHIShaderModule>> shaders(m_allocator);
    StlVector<ShaderReflection> reflections(m_allocator);
    size_t stages[0xF]{};
    for (auto const& [shader_path, entry_point, stage] : tracked.shaders) {
        LOG_RUNTIME(Renderer, debug, "Loading shader {}", shader_path.string());
        std::ifstream file(shader_path);
        CHECK(file.good());        
        data.resize(std::filesystem::file_size(shader_path));
        file.read(data.data(), data.size());
        auto& refl = reflections.emplace_back(data, m_allocator);
        // Check matching stage and entry point
        CHECK(refl.m_entrypoint.name == entry_point);
        CHECK(refl.m_entrypoint.stage == stage);
        LOG_RUNTIME(Renderer, debug, "{}", refl.DbgDumpShaderInfo());
        shaders.push_back(m_device->CreateShaderModule({ .source = data }));
        // In BindShader we have already guaranteed these to be unique per stage
        stages[(uint32_t)stage] = shaders.size() - 1;        
        pso_stages.push_back({
            .desc = {.stage = stage, .entry_point = entry_point.c_str() },
            .shader_module = shaders.back()
        });
    }
    // Check variable bindings to be consistent across stages
    StlMap<std::string, std::pair<uint32_t, uint32_t>> var_bindpoints(m_allocator);
    // Check if any shader in the pipeline uses PC
    bool use_pushconstants = false;
    for (auto& refl : reflections) {
        if (refl.m_pushConstants.size())
            use_pushconstants = true;        
        for (auto& bind : refl.m_bindings) {
            CHECK(bind.name.size() && "Unnamed bindings are not supported. Enable debug information.");
            auto it = var_bindpoints.find(bind.name);
            if (it == var_bindpoints.end())
                var_bindpoints[bind.name] = { bind.descriptorSet, bind.binding };
            else {
                auto& [set, binding] = it->second;
                CHECK(set == bind.descriptorSet && binding == bind.binding && "Inconsistent binding points across shader stages.");
            }
        }
    }
    // Create descriptor set layout to be consistent across stages
    StlMap<std::string, RHIDescriptorType> var_types(m_allocator);
    for (auto& [vhdl, dtype, binding] : tracked.tex_bindings) {
        auto& view = m_setup->trackedViews[vhdl];
        auto it = var_types.find(binding);
        if (it == var_types.end())
            var_types[binding] = dtype;
        else {
            auto& dtype_prev = it->second;
            CHECK(dtype_prev == dtype && "Inconsistent descriptor types across bindings.");
        }
    }
    for (auto& [rhdl, dtype, binding] : tracked.buf_bindings) {
        auto it = var_types.find(binding);
        if (it == var_types.end())
            var_types[binding] = dtype;
        else {
            auto& dtype_prev = it->second;
            CHECK(dtype_prev == dtype && "Inconsistent descriptor types across bindings.");
        }
    }
    LOG_RUNTIME(Renderer, debug, "Pipeline Parameters");
    for (auto& [name, dtype] : var_types) {
        auto [set, binding] = var_bindpoints[name];
        LOG_RUNTIME(Renderer, debug, "\t{}: set {}, binding {}, type {}", name, set, binding, dtype);
    }
    StlVector<std::pair<std::pair<uint32_t, uint32_t>, std::string>> bindings(m_allocator);
    bindings.reserve(var_types.size());
    for (auto& [name, bind] : var_bindpoints)
        bindings.push_back({ bind, name });
    std::sort(bindings.begin(), bindings.end());
    // Seperate into descriptor sets
    StlVector<RHIDeviceDescriptorSetLayoutDesc::Binding> set_bindings(m_allocator);
    for (auto& [_, binding] : bindings) {
        // !! TODO: Descriptor Arrays
        set_bindings.push_back({ .count = 1, .stage = RHIShaderStageBits::All, .type = var_types[binding] });
    }
    for (size_t i = 0, j = 0; i < bindings.size(); i = j) {
        uint32_t set = bindings[i].first.first;
        while (j < bindings.size() && bindings[j].first.first == set) j++;
        tracked.desc_layouts.push_back(m_device->CreateDescriptorSetLayout(
            { .bindings = { set_bindings.cbegin() + i, set_bindings.cbegin() + j } }
        ));
        tracked.desc_sets.push_back(m_descPool->CreateDescriptorSet(tracked.desc_layouts.back()));
        tracked.p_desc_sets.push_back(tracked.desc_sets.back().Get());
    }
    RHIPipelineState::PipelineStateDesc pso_desc{
        .vertex_input = tracked.vertex_input,
        .topology = RHIPipelineState::PipelineStateDesc::TRIANGLE_LIST,
        .rasterizer = {
            .fill_mode = RHIPipelineState::PipelineStateDesc::Rasterizer::FILL_SOLID,
            .cull_mode = RHIPipelineState::PipelineStateDesc::Rasterizer::CULL_BACK,
            .front_face = RHIPipelineState::PipelineStateDesc::Rasterizer::FF_COUNTER_CLOCKWISE,
        },
        .multisample = {.enabled = false },
        .depth_stencil = {                        
            .depth_test = true,
            .depth_write = true
        },
        .shader_stages = pso_stages,
        .descriptor_set_layouts = tracked.desc_layouts,
        .push_constants = tracked.push_constants
    };
    if (tracked.push_constants.size())
        CHECK(use_pushconstants && "Push constants set but never used. Possible shader error.")
    // Setup compute/graphics specific states
    // Graphics
    // RTV,DSV
    StlVector<RHIPipelineState::PipelineStateDesc::Attachment> attachments(m_allocator);
    for (auto rtv : tracked.rtvs) {
        auto& [rhdl, desc] = m_setup->trackedViews[rtv];
        attachments.push_back({ .render_target = { .format = desc.format } });
    }
    pso_desc.attachments = attachments;
    pso_desc.depth_stencil = {
        .depth_test = tracked.dsv != kInvalidHandle,
        .depth_write = tracked.dsv != kInvalidHandle,
        .depth_compare_op = RHIPipelineState::PipelineStateDesc::DepthStencil::CompareOp::LESS,
    };
    if (tracked.dsv != kInvalidHandle) {
        auto& [dhdl, desc] = m_setup->trackedViews[tracked.dsv];
        pso_desc.depth_stencil.depth_format = desc.format;
        // TODO Stencil?
    }
    tracked.pso = m_device->CreatePipelineState(pso_desc);
}

void Renderer::FinalizePSOs() {
    CHECK(m_state == State::Setup);
    // Reset descriptor pool
    m_descPool.Reset();
    StlVector<RHIDeviceDescriptorPool::PoolDesc::Binding> bindings(m_allocator);
    bindings.reserve(m_setup->binding_counts.size());
    LOG_RUNTIME(Renderer, debug, "Descriptor Pool");
    for (auto& [type, count] : m_setup->binding_counts) {
        LOG_RUNTIME(Renderer, debug, "\t{}: {}", type, count);
        bindings.push_back({ .type = type, .max_count = count });
    }
    m_descPool = m_device->CreateDescriptorPool({ bindings });
    // Build PSOs for everything we need
    for (auto& pass : m_setup->trackedPasses) {
        if (!pass.used) continue;
        BuildPipelineState(pass.handle);
    }
}
void Renderer::FinalizeResources() {
    CHECK(m_state == State::Setup);
    m_resources = ConstructUnique<Resources>(m_allocator, m_allocator);
    m_resources->fit(m_setup->trackedResources.size());
    // Create semaphores for inter-queue synchronization
    // Optionally used to be actually waited on (inter-queue, etc), but will always be signaled
    for (PassHandle ord = 0; ord < m_setup->execution.size(); ord++) {
        auto& pass = m_setup->trackedPasses[m_setup->execution[ord]];
        if (pass.has_cross_queue_dependent)
            pass.waitSemaphore = m_device->CreateSemaphore(true);
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
    for (PassHandle ord = 0; ord < m_setup->execution.size(); ord++) {
        auto& pass = m_setup->trackedPasses[m_setup->execution[ord]];
        for (auto hdl : pass.texviews)
            activeViews.push_back(hdl);
    }
    // Instantiate active ones only
    std::sort(activeViews.begin(), activeViews.end());
    activeViews.erase(std::unique(activeViews.begin(), activeViews.end()), activeViews.end());
    m_resources->fit(activeViews.size());
    for (auto hdl : activeViews) {
        auto [rhdl, desc] = m_setup->trackedViews[hdl];
        auto& res = DerefResource(rhdl).Get<RHIDeviceObjectHandle<RHITexture>>();
        m_resources->views[hdl] = res->CreateTextureView(desc);
    }
    // Reset resource states
    for (auto& res : m_setup->trackedResources) {
        res.lastBufferState.reset();
        for (auto& sta : res.lastSubresourceStates)
            sta.reset();
    }
}
void Renderer::ExecuteBarriers(TrackedPass& pass, RHICommandList* cmd) {
    CHECK(m_state == State::Execute);
    // At this point the pass execution order has been determined
    // (execution) and so are the resources' access patterns.
    // Minimal synchronization barriers would always be the most
    // optimal.
    // Inter-queue synchronization is handled separately at SubmitPass
    // where we wait on the producer pass's semaphore on the GPU
    cmd->BeginTransition();
    // Textures
    // These are always disjoint ranges
    for (auto [hdl, access, stage, range, layout] : pass.textureUsages) {
        auto& tres = m_setup->trackedResources[hdl];
        auto& res = DerefResource(hdl).Get<RHIDeviceObjectHandle<RHITexture>>();
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
        auto& res = DerefResource(hdl).Get<RHIDeviceObjectHandle<RHIBuffer>>();
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
bool Renderer::ExecuteSubmitOrContinue(TrackedPass& pass, RHICommandList* cmd) {
    CHECK(m_state == State::Execute);
    if (m_enableAsyncCompute) {
        Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
        // Pass that depends on a producer pass that's on another queue
        // needs external synchronization.    
        Core::StlVector<std::pair<RHIDeviceSemaphore*, size_t>> waits(&alloc);
        auto check_wait = [&](PassHandle other) {
            if (other != kInvalidHandle) {
                auto& opass = m_setup->trackedPasses[other];
                if (opass.queue != pass.queue) {
                    CHECK(opass.waitSemaphore.IsValid() && "Pass contains invalid wait semaphore");
                    // Wait on the producer pass's semaphore                       
                    waits.emplace_back(opass.waitSemaphore.Get(), SEM_COUNTER(opass.ord));
                }
            }
        };
        // Textures
        for (auto const& [hdl, access, stage, range, layout] : pass.textureUsages) {
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
        for (auto const& [hdl, access, stage] : pass.bufferUsages) {
            auto& res = m_setup->trackedResources[hdl];
            if (res.lastBufferState.producer == pass.handle)
                continue;
            check_wait(res.lastBufferState.producer);
            if (access & kAllShaderWrites)
                res.lastBufferState.producer = pass.handle;
        }
        // Submit
        // If we'd need any kind of cross queue syncs, this must be ended and submitted now.
        if (waits.size() || pass.has_cross_queue_dependent) {
            // Submit to appropriate queue
            cmd->DebugEnd().End();
            RHIDeviceQueue* queue = pass.queue == RHIDevicePipelineType::Graphics ? m_gfxQueue : m_compQueue;
            queue->Submit({
                .timeline_waits = waits,
                .timeline_signals = {{{ pass.waitSemaphore.Get(), SEM_COUNTER(pass.ord) }}},
                .cmd_lists = { cmd },
            });
            return true;
        }
        else {
            // Otherwise continue recording on the same command list
            return false;
        }
    }
    else {
        // Without async compute.
        // One large command list would be enough and in fact optimal.
        return false;
    }
}
#pragma endregion


void Renderer::Execute() {
    CHECK(m_state == State::PostSetup && "Invalid state. Did you call EndSetup()?");
    m_state = State::Execute;
    // Execute passes
    auto& ords = m_setup->execution;
    auto& passes = m_setup->trackedPasses;
    // See Also
    // - https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/03_Drawing/03_Frames_in_flight.html    
    // - https://docs.vulkan.org/tutorial/latest/_attachments/16_frames_in_flight.cpp
    m_device->WaitForFences({ m_swaps[m_currentSwap].fence }, true, -1);
    uint32_t next_image = m_swapchain->GetNextImage(-1, m_swaps[m_currentSwap].present, {});
    m_device->ResetFences({ m_swaps[m_currentSwap].fence });
    auto& cmds = m_swaps[m_currentSwap].cmds;
    uint32_t cmd_index = 0;
    auto* cmd = cmds[cmd_index].Get();
    cmd->Reset();
    cmd->Begin().DebugBegin();
    for (size_t ord : ords) {
        auto& pass = passes[ord];
        cmd->DebugInsertMarker(pass.name.c_str());
        ExecuteBarriers(pass, cmd);
        pass.pass->Record(pass.handle, *this, cmd);
        // Submit if needed
        if (ExecuteSubmitOrContinue(pass, cmd)) {
            // Record on a new one per swap
            CHECK(cmd_index < kMaxCommandListsPerSwap && "Command Lists overflow");
            cmd = cmds[++cmd_index].Get();
            cmd->Reset();
            cmd->Begin().DebugBegin();
        }
    }
    // Always transition swapchain image to present
    cmd->BeginTransition();
    cmd->SetImageTransition(
        m_swapchain->GetImages()[m_currentSwap],
        RHICommandList::TransitionDesc{
            .dst_stage = RHIPipelineStageBits::BottomOfPipe,
            .dst_img_layout = RHITextureLayout::Present
        }
    );
    cmd->EndTransition();
    cmd->DebugEnd().End();
    // Submit final command list
    // always on the most competent queue
    m_gfxQueue->Submit({
        .waits =  {{ m_swaps[m_currentSwap].present.Get() }},
        .signals = {{ m_swaps[m_currentSwap].render.Get() }},
        .cmd_lists = {{ cmd }},
        .fence = m_swaps[m_currentSwap].fence.Get()
    });
    m_gfxQueue->Present({
        .image_index = next_image,
        .swapchain = m_swapchain.Get(),
        .waits = {{ m_swaps[m_currentSwap].render.Get() }}
    });   
    m_currentSwap = (m_currentSwap + 1) % m_frameSwaps;
    m_frame++;
    m_state = State::PostSetup;
}
void Renderer::CmdSetPushConstant(RHICommandList* cmd, PassHandle pass, ResourceHandle push_constant, size_t size, void* data)
{
    CHECK(m_state == State::Execute);
    auto& tpass = m_setup->trackedPasses[pass];
    auto& [pc_stage, pc_offset, pc_size] = tpass.push_constants[push_constant];
    CHECK(pc_size >= size && "Set value larger than speicified");    
    cmd->PushConstant(tpass.pso.Get(), pc_stage, pc_offset, { static_cast<char*>(data), size });
}
void Renderer::CmdSetPipeline(RHICommandList* cmd, PassHandle pass) {
    CHECK(m_state == State::Execute);
    auto& tpass = m_setup->trackedPasses[pass];
    CHECK(tpass.pso.IsValid() && "Current pass has no Pipeline state.");
    cmd->SetPipeline({
        .pipeline = tpass.pso.Get(),
        .type = tpass.queue
    });    
    cmd->BindDescriptorSet(
        tpass.queue,
        tpass.pso.Get(),
        tpass.p_desc_sets
    );
}
