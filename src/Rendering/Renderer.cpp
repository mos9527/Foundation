// !! TODO: Per-swap resources
#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>


#include <Math/Math.hpp>
#include <Core/Allocator/StackAllocator.hpp>
#include <RHICore/Device.hpp>

#include "Renderer.hpp"
#include "ShaderReflection.hpp"
#include "spdlog/fmt/bundled/compile.h"

using namespace Foundation::Core;
using namespace Foundation::Rendering;
// Semaphore counter
#define SEM_COUNTER(ord) m_frame + ord + 1LL

Renderer::Renderer(RendererDesc const& desc, RHIApplicationObjectHandle<RHIDevice> device, RHIDeviceObjectHandle<RHISwapchain> swapchain, Core::Allocator* allocator)
    : m_device(device), m_allocator(allocator), m_state(State::Undefined), m_desc(desc) {
    m_gfxQueue = m_device->GetDeviceQueue(RHIDeviceQueueType::Graphics);
    m_compQueue = m_device->GetDeviceQueue(RHIDeviceQueueType::Compute);
    m_cmdPool = m_device->CreateCommandPool(RHICommandPool::PoolDesc{
        .queue = RHIDeviceQueueType::Graphics,
        .type = RHICommandPoolType::Persistent
    });
    m_cmdPool->DebugSetObjectName("Main Command Pool");
    if (m_desc.async) {
        m_compCmdPool = m_device->CreateCommandPool(RHICommandPool::PoolDesc{
            .queue = RHIDeviceQueueType::Compute,
            .type = RHICommandPoolType::Persistent
            });
        m_compCmdPool->DebugSetObjectName("Async Compute Command Pool");
    }
    if (m_desc.present)
        SetSwapchain(swapchain);
    else
        SetFrameSyncObjects();
    LOG_RUNTIME(Renderer, info, "** Renderer Init **");
    LOG_RUNTIME(Renderer, info, "Async Compute: {}", m_desc.async);
    LOG_RUNTIME(Renderer, info, "Presentation: {}", m_desc.present);
}

#pragma region Render Graph Setup
void Renderer::BeginSetup() {
    CHECK_MSG(m_state == State::Undefined || m_state == State::PostSetup, "Renderer may only be setup once for its life time. Current state is {}", m_state);
    m_setup = ConstructUnique<Setup>(m_allocator, m_allocator);
    m_state = State::Setup;
}
ResourceHandle Renderer::CreateTextureView(
    PassHandle pass, ResourceHandle handle, RHITextureViewDesc const& desc) {
    CHECK(m_state == State::Setup);
    m_setup->trackedViews.emplace_back(handle, desc);
    ResourceHandle hdl = m_setup->trackedViews.size() - 1;
    m_setup->trackedPasses[pass].texviews.emplace_back(hdl);
    return hdl;
}
ResourceHandle Renderer::CreateSampler(std::string const& name, RHIDeviceSampler::SamplerDesc const& desc) {
    CHECK(m_state == State::Setup);
    m_setup->trackedSamplers.emplace_back(desc);
    return m_setup->trackedSamplers.size() - 1;
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
    std::string const& entry_point,
    std::filesystem::path const& shader_path
) {
    CHECK(m_state == State::Setup);
    CHECK_MSG(stage.is_bitmask(), "Only one stage can be bound to a shader per pass");
    for (auto& [_, ep, st] : m_setup->trackedPasses[pass].shaders)
        if (st & stage)
            throw std::runtime_error("Some previous shader stage(s) already bound to a shader");
    m_setup->trackedPasses[pass].shaders.emplace_back(shader_path, entry_point, stage);
}
void Renderer::BindVertexInput(
    PassHandle pass,
    RHIPipelineState::PipelineStateDesc::VertexInput const& info
) {
    CHECK(m_state == State::Setup);
    m_setup->trackedPasses[pass].vertex_input = info;
}
void Renderer::BindPushConstant(
    PassHandle pass, RHIShaderStage stage,
    size_t offset, size_t size
) {
    CHECK(m_state == State::Setup);
    for (auto const& [s, _, __] : m_setup->trackedPasses[pass].push_constants)
        if (s & stage)
            throw std::runtime_error("Some previous shader stage(s) already has Push Constants ranges");
    m_setup->trackedPasses[pass].push_constants.emplace_back(stage, offset, size);
}     
void Renderer::BindBufferUniform(
    PassHandle pass, ResourceHandle buffer,
    std::string const& bind_point
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
    std::string const& bind_point
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
    std::string const& bind_point
) {
    CHECK(m_state == State::Setup);
    DeclareBufferAccess(pass, buffer,
        kAllPipelineShaderStages,
        RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite
    );
    m_setup->trackedPasses[pass].buf_bindings.emplace_back(buffer, RHIDescriptorType::StorageBuffer, bind_point);
    m_setup->binding_counts[RHIDescriptorType::StorageBuffer]++;
}
void Renderer::BindBufferShaderRead(PassHandle pass, ResourceHandle buffer) {
    CHECK(m_state == State::Setup);
    DeclareBufferAccess(pass, buffer,
        kAllPipelineShaderStages,
        RHIResourceAccessBits::ShaderRead
    );
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
void Renderer::BindTextureSampler(
    PassHandle pass, ResourceHandle sampler,
    std::string const& shader_name
) {
    CHECK(m_state == State::Setup);
    m_setup->trackedPasses[pass].samplers.emplace_back(sampler, shader_name);
    m_setup->binding_counts[RHIDescriptorType::Sampler]++;
}
ResourceHandle Renderer::BindTextureSRV(
    PassHandle pass, ResourceHandle texture,
    std::string const& shader_name,
    RHITextureViewDesc const& desc
) {
    CHECK(m_state == State::Setup);
    RHIPipelineStage stages{};
    switch (m_setup->trackedPasses[pass].queue)
    {
    case RHIDeviceQueueType::Graphics:
        stages = kAllPipelineShaderStages;
        break;
    case RHIDeviceQueueType::Compute:
        stages = RHIPipelineStageBits::ComputeShader;
        break;
    default:
        CHECK_MSG(false, "Unsupported queue");
    }
    DeclareTextureAccess(pass, texture,
        stages,
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
    std::string const& shader_name,
    RHITextureViewDesc const& desc
) {
    CHECK(m_state == State::Setup);
    RHIPipelineStage stages{};
    switch (m_setup->trackedPasses[pass].queue)
    {
    case RHIDeviceQueueType::Graphics:
        stages = kAllPipelineShaderStages;
        break;
    case RHIDeviceQueueType::Compute:
        stages = RHIPipelineStageBits::ComputeShader;
        break;
    default:
        CHECK_MSG(false, "Unsupported queue");
    }
    DeclareTextureAccess(pass, texture,
        stages,
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
    CHECK_MSG(tpass.queue == RHIDeviceQueueType::Graphics, "RTV (Render Target Views) are only supported on Graphics queues");
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
    CHECK_MSG(tpass.queue == RHIDeviceQueueType::Graphics, "DSV (Depth Stencil Views) are only supported on Graphics queues");
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
void Renderer::BindBackbufferRTV(PassHandle pass) {
    CHECK(m_state == State::Setup);
    auto& tpass = m_setup->trackedPasses[pass];
    CHECK_MSG(tpass.queue == RHIDeviceQueueType::Graphics, "RTV (Render Target Views) are only supported on Graphics queues");
    tpass.write_backbuffer = true;
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
void Renderer::EndSetup() {
    CHECK_MSG(m_state == State::Setup, "Bad renderer state ({}). Did you call BeginSetup()?", m_state);
    if (m_setup->trackedPasses.size()) {
        // Setup all passes
        for (PassHandle i = 0; i < m_setup->trackedPasses.size(); ++i) {
            auto& pass = m_setup->trackedPasses[i];
            pass.pass->Setup(pass.handle, this);
        }
        CullPasses(m_setup->epilogue);
        FinalizeResources();
        FinalizePSOs();
    }
    else {
        LOG_RUNTIME(Renderer, warn, "No passes created in render graph.");
    }
    m_state = State::PostSetup;
}
void Renderer::CullPasses(PassHandle epilogue) {
    CHECK(m_state == State::Setup);
    CHECK(epilogue < m_setup->trackedPasses.size());
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
            CHECK_MSG(vis[v] != 1, "Cyclic dependency in graph.");
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
        m_setup->trackedPasses[epilogue].used = true;
    }
    m_setup->epilogue = epilogue;
    for (PassHandle ord = 0; ord < m_setup->execution.size(); ord++) {
        auto& pass = m_setup->trackedPasses[m_setup->execution[ord]];
        // Derive lifetimes for resources from execution order
        // FinalizeResources() uses this to overlap resources.
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
    LOG_RUNTIME(Renderer, debug, "** Render Graph GraphViz **\n{}", DbgDumpGraphviz());
    LOG_RUNTIME(Renderer, debug, "** Render Graph Execution Order **\n{}", DbgDumpActivePasses());
}
void Renderer::BuildPipelineState(PassHandle pass) {
    auto& tracked = m_setup->trackedPasses[pass];
    StlVector<RHIPipelineState::PipelineStateDesc::ShaderStage> pso_stages(m_allocator);
    // Load shader bytecode
    if (!tracked.shaders.size())
        return; // Pass with no shaders
    LOG_RUNTIME(Renderer, debug, "** Building PSO for {} [{}] **", tracked.name, pass);
    StlVector<char> data(m_allocator);
    StlMap<std::filesystem::path, RHIDeviceScopedObjectHandle<RHIShaderModule>> shaders(m_allocator);
    StlMap<std::filesystem::path, UniquePtr<ShaderReflection>> reflections(m_allocator);
    for (auto const& [shader_path, entry_point, stage] : tracked.shaders) {
        if (!shaders.contains(shader_path)) {
            LOG_RUNTIME(Renderer, debug, "Loading shader {}", shader_path.string());
            std::ifstream file(shader_path, std::ios::binary);
            CHECK_MSG(file.good(), "Failed to open shader file {}", shader_path.string());
            data.resize(std::filesystem::file_size(shader_path));
            file.read(data.data(), data.size());
            CHECK_MSG(file.gcount() == data.size(), "Shader read failure. Read {} bytes, expected {}", file.gcount(), data.size());
            // Verifiy shader stage
            auto const& refl = reflections.emplace(shader_path, ConstructUnique<ShaderReflection>(m_allocator, data, m_allocator)).first->second;
            LOG_RUNTIME(Renderer, debug, "** Shader Info**\n{}", refl->DbgDumpShaderInfo());
            shaders[shader_path] = m_device->CreateShaderModule({ .source = data });
            shaders[shader_path]->DebugSetObjectName(shader_path.string().c_str());
        }
        auto& module = shaders[shader_path];
        // In BindShader we have already guaranteed these to be unique per stage
        if (stage == RHIShaderStageBits::Compute)
            tracked.compute_pass = true;
        bool found = false;
        for (auto const& ep : reflections[shader_path]->m_entrypoints) {
            if (ep.stage == stage && ep.name == entry_point) {
                pso_stages.push_back({
                    .desc = {.stage = stage, .entry_point = ep.name.c_str()},
                    .shader_module = module
                });
                if (stage == RHIShaderStageBits::Compute)
                    tracked.compute_local_size = ep.local_size;
                found = true;
                break;
            }
        }
        CHECK_MSG(found, "No entry point {} found for stage {} in shader {}", entry_point, stage, shader_path.string());
    }
    if (tracked.compute_pass) {
        CHECK_MSG(shaders.size() == 1, "Pass {} must have exactly 1 Compute Shader, and 0 of any other types, if CS is used.", tracked.name);
        CHECK_MSG(tracked.write_backbuffer == false, "Pass {} uses Compute Shader, and cannot write to the backbuffer.", tracked.name);
        CHECK_MSG(tracked.rtvs.size() == 0 && tracked.dsv == kInvalidHandle, "Pass {} uses Compute Shader, and cannot have RTVs or DSVs.", tracked.name);
    }
    // Check variable bindings to be consistent across stages
    // [name, [set, binding]]
    StlMap<std::string, std::pair<uint32_t, uint32_t>> var_bindpoints(m_allocator);
    // Check if any shader in the pipeline uses PC
    for (auto const& [path, refl] : reflections){
        if (refl->m_pushConstants.size())
            CHECK_MSG(refl->m_pushConstants.size() == 1, "Shader uses more than Push Constant block. This is not accepted by most drivers.");
        for (auto& bind : refl->m_bindings) {
            CHECK_MSG(
                bind.name.size(),
                "Unnamed bindings are not supported. Enable debug information for shader {}",
                path.string()
            );
            auto it = var_bindpoints.find(bind.name);
            if (it == var_bindpoints.end())
                var_bindpoints[bind.name] = { bind.descriptorSet, bind.binding };
            else {
                auto& [set, binding] = it->second;
                CHECK_MSG(
                    set == bind.descriptorSet && binding == bind.binding, 
                    "Inconsistent binding points across shader stages for variable {} in shader {}",
                    bind.name, path.string()
                );
            }
        }
    }
    // Create descriptor set layout to be consistent across stages
    StlMap<std::string, RHIDescriptorType> var_types(m_allocator);
    StlMap<std::string, ResourceHandle> var_hdls(m_allocator);
    StlMap<std::string, ResourceHandle> var_samplers(m_allocator);
    for (auto& [vhdl, dtype, binding] : tracked.tex_bindings) {
        auto it = var_types.find(binding);
        if (it == var_types.end())
            var_types[binding] = dtype, var_hdls[binding] = vhdl;
        else {
            auto& dtype_prev = it->second;
            auto& vhdl_prev = var_hdls[binding];
            CHECK(dtype_prev == dtype && vhdl_prev == vhdl);
        }
    }
    for (auto& [rhdl, dtype, binding] : tracked.buf_bindings) {
        auto it = var_types.find(binding);
        if (it == var_types.end())
            var_types[binding] = dtype, var_hdls[binding] = rhdl;
        else {
            auto& dtype_prev = it->second;
            auto& rhdl_prev = var_hdls[binding];
            CHECK(dtype_prev == dtype && rhdl_prev == rhdl);
        }
    }
    for (auto& [shdl, binding] : tracked.samplers) {
        auto it = var_types.find(binding);
        if (it == var_types.end())
            var_types[binding] = RHIDescriptorType::Sampler, var_samplers[binding] = shdl;
        else {
            auto& dtype_prev = it->second;
            auto& shdl_prev = var_samplers[binding];
            CHECK(dtype_prev == RHIDescriptorType::Sampler && shdl_prev == shdl);
        }
    }
    if (var_bindpoints.size()) {
        LOG_RUNTIME(Renderer, debug, "Pipeline Parameters");
        for (auto& [name, dtype] : var_types) {
            if (!var_bindpoints.contains(name))
                continue;
            auto [set, binding] = var_bindpoints[name];
            LOG_RUNTIME(Renderer, debug, "\t{}: set {}, binding {}, type {}", name, set, binding, dtype);
        }
    }
    // [[set, binding], name]
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
        tracked.desc_layouts.back()->DebugSetObjectName(
            fmt::format("Descriptor Set Layout {} of {} [{}]", set, tracked.name, pass).c_str()
        );
        CHECK_MSG(m_descPool.IsValid(), "Shader declared bindings, but the pass {} didn't provide any.", tracked.name);
        tracked.desc_sets.push_back(m_descPool->CreateDescriptorSet(tracked.desc_layouts.back()));
        auto& ds = tracked.desc_sets.back();
        ds->DebugSetObjectName(
            fmt::format("Descriptor Set {} of {} [{}]", set, tracked.name, pass).c_str()
        );
        tracked.p_desc_sets.push_back(ds.Get());
        // Update bindings
        LOG_RUNTIME(Renderer, debug, "Descriptor Set {} Bindings", set);
        for (size_t k = i; k < j; k++) {
            auto& [bind, name] = bindings[k];
            auto& [_, binding] = bind;
            auto& hdl = var_hdls[name];
            auto& type = var_types[name];
            using enum RHIDescriptorType;
            switch (type)
            {
            case Sampler:
                {
                    CHECK_MSG(var_samplers.contains(name), "Shader expects a Sampler at {}, but it's not bound by pass {}", name, tracked.name);
                    auto& shdl = var_samplers[name];
                    auto* sampler = DerefSampler(shdl);
                    LOG_RUNTIME(Renderer, debug, "\t[Sampler] {}: binding {}, type {}", name, binding, type);
                    ds->Update({
                        .binding = binding,
                        .type = type,
                        .images = {{{
                            .sampler = sampler
                        }}}
                        });                    
                    break;
            }
                case SampledImage:
                case StorageImage:
                {
                    auto* view = DerefTextureView(hdl);
                    LOG_RUNTIME(Renderer, debug, "\t[Texture] {}: binding {}, type {}", name, binding, type);
                    ds->Update({
                        .binding = binding,
                        .type = type,
                        .images = {{{
                            .image_view = view,
                            .layout = type == RHIDescriptorType::SampledImage ?
                                RHITextureLayout::ShaderReadOnly : RHITextureLayout::General
                        }}}
                    });
                    break;
                }
                case UniformBuffer:
                case StorageBuffer:
                {
                    LOG_RUNTIME(Renderer, debug, "\t[Buffer] {}: binding {}, type {}", name, binding, type);
                    auto* buf = DerefResource(hdl).Get<RHIBuffer*>();
                    ds->Update({
                        .binding = binding,
                        .type = type,
                        .buffers = {{{ .buffer = buf }}}
                    });
                    break;
                }
            default:
                break;
            }
        }
    }
    RHIPipelineState::PipelineStateDesc pso_desc{
        .type = tracked.compute_pass ? RHIDevicePipelineType::Compute : RHIDevicePipelineType::Graphics,
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
    // Setup compute/graphics specific states
    // Graphics
    // RTV,DSV
    StlVector<RHIPipelineState::PipelineStateDesc::Attachment> attachments(m_allocator);
    if (tracked.write_backbuffer) {
        // Only write to the backbuffer
        attachments.push_back({ .render_target = {.format = m_swapchain->m_desc.format } });
    } else{
        for (auto rtv : tracked.rtvs) {
            auto& [rhdl, desc] = m_setup->trackedViews[rtv];
            attachments.push_back({ .render_target = {.format = desc.format } });
        }
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
    tracked.pso->DebugSetObjectName(fmt::format("PSO of {} [{}]", tracked.name, pass).c_str());
}
void Renderer::FinalizePSOs() {
    CHECK(m_state == State::Setup);
    // Build descriptor pool
    m_descPool.Reset();
    if (m_setup->binding_counts.size()) {
        StlVector<RHIDeviceDescriptorPool::PoolDesc::Binding> bindings(m_allocator);
        bindings.reserve(m_setup->binding_counts.size());
        LOG_RUNTIME(Renderer, debug, "** Descriptor Pool **");
        for (auto& [type, count] : m_setup->binding_counts) {
            LOG_RUNTIME(Renderer, debug, "\t{}: {}", type, count);
            bindings.push_back({ .type = type, .max_count = count });
        }
        m_descPool = m_device->CreateDescriptorPool({ bindings });
        m_descPool->DebugSetObjectName("Renderer Descriptor Pool");
    }
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
        if (m_desc.async && pass.has_cross_queue_dependent) {
            pass.asyncSemaphore = m_device->CreateSemaphore(true);
            pass.asyncSemaphore->DebugSetObjectName(
                fmt::format("Timeline Semaphore of {} [{}]", pass.name, pass.handle).c_str()
            );
        }
    }
    // !! TODO: Overlap transient resources to with non-overlapping lifetimes with aliasing
    for (auto& [handle, _] : m_setup->activeResources) {
        auto& res = m_setup->trackedResources[handle];
        res.desc.visit(
            // Owned
            [&](RHIBufferDesc const& desc) {                
                m_resources->resources[handle] = m_device->CreateBuffer(desc);
                DerefResource(handle).Get<RHIBuffer*>()->DebugSetObjectName(
                    fmt::format("{} [{}]", res.name, handle).c_str()
                );
            },
            [&](RHITextureDesc const& desc) {
                m_resources->resources[handle] = m_device->CreateTexture(desc);
                DerefResource(handle).Get<RHITexture*>()->DebugSetObjectName(
                    fmt::format("{} [{}]", res.name, handle).c_str()
                );
            },
            // Borrowed
            [&](RHIDeviceObjectHandle<RHIBuffer> const& hdl) { m_resources->resources[handle] = hdl; },
            [&](RHIDeviceObjectHandle<RHITexture> const& hdl) { m_resources->resources[handle] = hdl; }
        );
    }
    // Create texture views
    StlVector<ResourceHandle> activeViews(m_allocator), activeSamplers(m_allocator);
    for (PassHandle ord = 0; ord < m_setup->execution.size(); ord++) {
        auto& pass = m_setup->trackedPasses[m_setup->execution[ord]];
        for (auto hdl : pass.texviews)
            activeViews.push_back(hdl);
        for (auto hdl : pass.samplers)
            activeSamplers.push_back(hdl.first);
    }
    // Instantiate views
    std::sort(activeViews.begin(), activeViews.end());
    activeViews.erase(std::unique(activeViews.begin(), activeViews.end()), activeViews.end());
    m_resources->fit(activeViews.size());
    for (auto hdl : activeViews) {
        auto [rhdl, desc] = m_setup->trackedViews[hdl];
        auto& res = DerefResource(rhdl).Get<RHITexture*>();
        m_resources->views[hdl] = res->CreateTextureView(desc);
    }
    // Instantiate samplers
    std::sort(activeSamplers.begin(), activeSamplers.end());
    activeSamplers.erase(std::unique(activeSamplers.begin(), activeSamplers.end()), activeSamplers.end());
    m_resources->fit(activeSamplers.size());
    for (auto hdl : activeSamplers) {
        auto& desc = m_setup->trackedSamplers[hdl];
        m_resources->samplers[hdl] = m_device->CreateSampler(desc);
    }
    // Reset resource states
    for (auto& res : m_setup->trackedResources) {
        res.lastBufferState.reset();
        for (auto& sta : res.lastSubresourceStates)
            sta.reset();
    }
}
RHIPipelineStage Renderer::ExecuteGetPassAllCurrentStages(TrackedPass& pass) {
    RHIPipelineStage ans{};
    for (auto [hdl, access, stage, range, layout] : pass.textureUsages) {
        auto& tres = m_setup->trackedResources[hdl];
        for (auto& sta : tres.GetLastSubresourceStateOf(range))
            ans |= sta.stage;
    }
    for (auto [hdl, access, stage] : pass.bufferUsages)
        ans |= stage;
    return ans;
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
        auto* res = DerefResource(hdl).Get<RHITexture*>();
        for (auto& sta : tres.GetLastSubresourceStateOf(range)) {
            if (sta.access == access && sta.stage == stage && sta.layout == layout)
                continue;
            cmd->SetImageTransition(
                res,
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
        if (tres.lastBufferState.access == access && tres.lastBufferState.stage == stage)
            continue;
        auto& res = DerefResource(hdl).Get<RHIBuffer*>();
        cmd->SetBufferTransition(
            res,
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
bool Renderer::ExecuteSubmitOrContinue(TrackedPass& pass, RHICommandList* cmd, RHIDeviceQueue* queue, StlSpan<const std::pair<RHIDeviceSemaphore*, size_t>> extra_waits) {
    CHECK(m_state == State::Execute);
    if (m_desc.async) {
        Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
        // Pass that depends on a producer pass that's on another queue
        // needs external synchronization.    
        Core::StlVector<std::pair<RHIDeviceSemaphore*, size_t>> waits(&alloc);
        auto check_wait = [&](PassHandle other) {
            if (other != kInvalidHandle) {
                auto& opass = m_setup->trackedPasses[other];
                if (opass.queue != pass.queue) {
                    CHECK_MSG(opass.asyncSemaphore.IsValid(), "Pass {} [{}] is not valid to be waited on", opass.name, other);
                    // Wait on the producer pass's semaphore                       
                    waits.emplace_back(opass.asyncSemaphore.Get(), SEM_COUNTER(opass.ord));
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
        waits.insert(waits.end(), extra_waits.begin(), extra_waits.end());
        if (waits.size() || pass.has_cross_queue_dependent) {
            cmd->End();
            if (pass.has_cross_queue_dependent)
                queue->Submit({
                    .timeline_waits = waits,
                    .timeline_signals = {{{ pass.asyncSemaphore.Get(), SEM_COUNTER(pass.ord) }}},
                    .cmd_lists = { cmd },
                });
            else
                queue->Submit({
                    .timeline_waits = waits,
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
void Renderer::SetFrameSyncObjects() {
    for (size_t i = 0; i < m_frameSwaps; i++) {
        m_swaps[i].render = m_device->CreateSemaphore(false);
        m_swaps[i].render->DebugSetObjectName(fmt::format("Render Semaphore of Swap {}", i).c_str());
        m_swaps[i].present = m_device->CreateSemaphore(false);
        m_swaps[i].present->DebugSetObjectName(fmt::format("Present Semaphore of Swap {}", i).c_str());
        m_swaps[i].fence = m_device->CreateFence(true);
        m_swaps[i].fence->DebugSetObjectName(fmt::format("Fence of Swap {}", i).c_str());
    }
}
void Renderer::SetSwapchain(RHIDeviceObjectHandle<RHISwapchain> swapchain) {
    CHECK_MSG(m_desc.present, "Cannot set swapchain when the renderer is not declared with Present support");
    m_frameSwaps = swapchain->GetImages().size();
    LOG_RUNTIME(Renderer, info, "Swapchain uses {} back buffers", m_frameSwaps);
    if (m_state == State::Execute) {
        // If changing swapchain during execution
        // Wait for GPU to be idle
        m_device->WaitIdle();
        m_state = State::PostSetup;
    }
    for (size_t i = 0; i < m_frameSwaps; ++i) {
        auto* backbuffer = swapchain->GetImages()[i];
        backbuffer->DebugSetObjectName(fmt::format("Backbuffer of Swap {}", i).c_str());
        m_swaps[i].rtv = backbuffer->CreateTextureView(RHITextureViewDesc{
            .format = swapchain->m_desc.format
        });
        if (m_swaps[i].rt_handle == kInvalidHandle) {
            // First time setup
            m_swaps[i].rt_handle = CreateResource(
                fmt::format("Backbuffer of Swap {}", i),
                backbuffer
            );
        }
        else {
            // Update existing handle
            auto& rt_res = m_resources->resources[m_swaps[i].rt_handle];
            CHECK_MSG(rt_res.GetIf<RHITexture*>(), "Swapchain backbuffer handle {} is not a texture", m_swaps[i].rt_handle);
            rt_res = backbuffer;
        }
    }
    m_swapchain = swapchain;
    SetFrameSyncObjects();
    // Reset semaphores index
    m_currentSync = 0;
}

/**
 * @note MangoHud seem to cause validation errors with present semaphores
 */
void Renderer::Execute() {
    CHECK_MSG(m_state == State::PostSetup, "Renderer bad state ({}). Did you call EndSetup()?", m_state);
    m_state = State::Execute;
    // Execute passes
    auto& passes = m_setup->trackedPasses;
    m_device->WaitForFences({ m_swaps[m_currentSync].fence }, true, -1);
    if (m_desc.present)
        m_currentSwap = m_swapchain->GetNextImage(-1, m_swaps[m_currentSync].present, {});
    m_device->ResetFences({ m_swaps[m_currentSync].fence });
    int cmd_index = 0, cmd_comp_index = 0;
    RHIDeviceQueue* queue = m_gfxQueue;
    RHICommandList* cmd = m_swaps[m_currentSync].cmd_at(cmd_index, m_cmdPool.Get());
    auto set_next_graphics = [&]() {
        CHECK_MSG(cmd_index < kMaxCommandListsPerSwap, "Not enough graphics command lists allocated for async compute execution [graphics exhausted]");
        queue = m_gfxQueue, cmd = m_swaps[m_currentSync].cmd_at(cmd_index++, m_cmdPool.Get());
    };
    auto set_next_compute = [&]() {
        CHECK_MSG(cmd_comp_index < kMaxCommandListsPerSwap, "Not enough compute command lists allocated for async compute execution [compute exhausted]");
        queue = m_compQueue, cmd = m_swaps[m_currentSync].comp_cmds_at(cmd_comp_index++, m_compCmdPool.Get());
    };
    auto set_next_pass_queue = [&](TrackedPass& pass) {
        if (m_desc.async) {
            switch (pass.queue)
            {
            case RHIDeviceQueueType::Graphics:
                set_next_graphics();
                break;
            case RHIDeviceQueueType::Compute:
                set_next_compute();
                break;
            }
        }
        else {
            set_next_graphics();
        }
    };
    if (m_setup->execution.size()) {
        set_next_pass_queue(passes[m_setup->execution[0]]);
        cmd->Reset();
        cmd->Begin();
    }
    for (size_t i = 0; i < m_setup->execution.size();i++)
    {
        auto& pass = passes[m_setup->execution[i]];
        if (pass.write_backbuffer)
        {
            CHECK_MSG(m_desc.present, "Pass {} writes to the backbuffer, but the renderer is not created with Present support.", pass.name);

            break;
        }
    }
    for (size_t i = 0; i < m_setup->execution.size();i++) {
        auto& pass = passes[m_setup->execution[i]];
        // Check if we can transition away on Compute
        // If not, Graphics must take over
        std::optional<std::pair<RHIDeviceSemaphore*, size_t>> barrier_extra_wait{};
        if (m_desc.async && pass.queue == RHIDeviceQueueType::Compute) {
            const RHIPipelineStage computeMask =
                RHIPipelineStageBits::FragmentShader |
                RHIPipelineStageBits::VertexShader |
                RHIPipelineStageBits::MeshShader |
                RHIPipelineStageBits::RayTracingShader;
            RHIPipelineStage stages = ExecuteGetPassAllCurrentStages(pass);
            if (stages & computeMask) {
                set_next_graphics();
                cmd->Reset();
                cmd->Begin();
                ExecuteBarriers(pass, cmd);
                cmd->End();
                barrier_extra_wait.emplace(m_swaps[i].barrier_semaphore_at(cmd_index - 1, m_device.Get()), SEM_COUNTER(cmd_index - 1));
                queue->Submit({
                    .timeline_signals = { barrier_extra_wait.value() },
                    .cmd_lists = { cmd },
                });
                cmd_comp_index--;
                set_next_compute();
            }
            else {
                ExecuteBarriers(pass, cmd);
            }
        } else {
            ExecuteBarriers(pass, cmd);
        }
        cmd->DebugBegin(pass.name.c_str());
        pass.pass->Record(pass.handle, this, cmd);
        cmd->DebugEnd();
        // Submit if needed
        bool submitted = false;
        if (barrier_extra_wait.has_value()) {
            submitted = ExecuteSubmitOrContinue(pass, cmd, queue, { barrier_extra_wait.value() });
            barrier_extra_wait.reset();
        }
        else {
            submitted = ExecuteSubmitOrContinue(pass, cmd, queue);
        }
        if (submitted) {
            if (i == m_setup->execution.size() - 1)
            {
                // Ending pass
                // Only possible if we'd end up here with a pass
                // that's only waiting on other queues
                CHECK(!pass.has_cross_queue_dependent);
                if (m_desc.present) {
                    set_next_graphics();
                    cmd->Reset();
                    cmd->Begin();
                }
            }
            else {
                // Previous command list has already been consumed
                // Start a new one for the next pass            
                set_next_pass_queue(passes[m_setup->execution[i + 1]]);                
                cmd->Reset();
                cmd->Begin();
            }
        }
    }
    if (m_desc.present)
    {
        if (queue != m_gfxQueue)
        {
            queue->Submit({
                .signals = {{ m_swaps[m_currentSync].render.Get() }},
                .cmd_lists = {{ cmd }},
                .fence = m_swaps[m_currentSync].fence.Get()
            });
            set_next_graphics();
        }
        cmd->BeginTransition();
        cmd->SetImageTransition(
            DerefResource(GetCurrentBackbuffer()).Get<RHITexture*>(),
            RHICommandList::TransitionDesc{
                .src_stage = RHIPipelineStageBits::ColorAttachmentOutput,
                .dst_stage = RHIPipelineStageBits::BottomOfPipe,
                .src_access =  RHIResourceAccessBits::RenderTargetWrite,
                .src_img_layout = RHITextureLayout::RenderTarget,
                .dst_img_layout = RHITextureLayout::Present
            }
        );
        cmd->EndTransition();
        cmd->End();
        queue->Submit({
            .waits = {{ m_swaps[m_currentSync].present.Get() }},
            .signals = {{ m_swaps[m_currentSwap].render.Get() }},
            .cmd_lists = {{ cmd }},
            .fence = m_swaps[m_currentSync].fence.Get()
        });
        queue->Present({
            .image_index = m_currentSwap,
            .swapchain = m_swapchain.Get(),
            .waits = {{ m_swaps[m_currentSwap].render.Get() }}
        });
    } else {
        queue->Submit({
            .signals = {{ m_swaps[m_currentSync].render.Get() }},
            .cmd_lists = {{ cmd }},
            .fence = m_swaps[m_currentSync].fence.Get()
        });
    }
    m_currentSync = (m_currentSync + 1) % m_frameSwaps;
    m_frame++;
    m_state = State::PostSetup;
}
void Renderer::CmdSetPipeline(PassHandle pass, RHICommandList* cmd) {
    CHECK(m_state == State::Execute);
    auto& tpass = m_setup->trackedPasses[pass];
    CHECK_MSG(tpass.pso.IsValid(), "Current pass has no Pipeline state.");
    cmd->SetPipeline({
        .pipeline = tpass.pso.Get(),
        .type = tpass.compute_pass ? RHIDevicePipelineType::Compute : RHIDevicePipelineType::Graphics
        });
    if (tpass.p_desc_sets.size())
        cmd->BindDescriptorSet(
            tpass.compute_pass ? RHIDevicePipelineType::Compute : RHIDevicePipelineType::Graphics,
            tpass.pso.Get(),
            tpass.p_desc_sets
        );
}
void Renderer::CmdBeginGraphics(PassHandle pass, RHICommandList* cmd,
    RHIExtent2D const& extent,
    std::optional<RHIClearColor> clear_rtv,
    std::optional<RHIClearDepthStencil> clear_dsv
) {
    CHECK(m_state == State::Execute);
    auto& tpass = m_setup->trackedPasses[pass];
    CHECK_MSG(tpass.pso.IsValid(), "Current pass has no Pipeline state.");
    StackArena<1024> arena; StackAllocatorSingleThreaded alloc(arena);
    StlVector<RHICommandList::GraphicsDesc::Attachment> rtvs(alloc.Ptr());
    if (tpass.write_backbuffer) {
        RHIExtent2D backsize = GetSwapchainExtent();
        CHECK_MSG(
            extent.x <= backsize.x && extent.y <= backsize.y,
            "Graphics extent too large for Swapchain Backbuffer {}",
            m_currentSync
        );
        rtvs.push_back({
            .image_view = DerefCurrentBackbufferView(pass),
            .clear_color = clear_rtv
        });
    }
    else {
        rtvs.reserve(tpass.rtvs.size());
        for (auto rtv : tpass.rtvs) {           
            auto& [rhdl, desc] = m_setup->trackedViews[rtv];
            auto& tres = m_setup->trackedResources[rhdl];
            auto& res = DerefResource(rhdl).Get<RHITexture*>();
            CHECK_MSG(
                res->m_desc.extent.x >= extent.x && res->m_desc.extent.y >= extent.y,
                "Graphics extent too large for Render Traget on {}", tres.name
            );
            rtvs.push_back({
                .image_view = DerefTextureView(rtv),
                .clear_color = clear_rtv
            });
        }
    }
    if (tpass.dsv != kInvalidHandle) {
        auto& [dhdl, desc] = m_setup->trackedViews[tpass.dsv];
        auto& tres = m_setup->trackedResources[dhdl];
        auto& res = DerefResource(dhdl).Get<RHITexture*>();
        CHECK_MSG(
            res->m_desc.extent.x >= extent.x && res->m_desc.extent.y >= extent.y,
            "Graphics extent too large for Depth buffer {}",
            tres.name
        );
        cmd->BeginGraphics({
            .color_attachments = rtvs,
            .depth_attachment = {.image_view = DerefTextureView(tpass.dsv), .clear_depth_stencil = clear_dsv },
            .width = extent.x,
            .height = extent.y
        });
    }
    else {
        CHECK_MSG(
            rtvs.size(), "No RTVs or DSV bound for graphics pass {} [{}]", tpass.name, pass
        )
        cmd->BeginGraphics({
            .color_attachments = rtvs,            
            .width = extent.x,
            .height = extent.y
        });
    }
}
RHIExtent3D Renderer::CmdGetComputeLocalSize(PassHandle pass) {
    CHECK(m_state == State::Execute);
    auto& tpass = m_setup->trackedPasses[pass];
    auto const& [x, y, z] = tpass.compute_local_size;
    CHECK_MSG(x > 0 && y > 0 && z > 0, "Pass {} does not have a valid compute local size", tpass.name);
    return { x,y,z };
}
void Renderer::CmdDispatch(
    PassHandle pass, RHICommandList* cmd,
    RHIExtent3D thread_size
) {
    CHECK(m_state == State::Execute);    
    auto const& local_size = CmdGetComputeLocalSize(pass);
    cmd->Dispatch(
        (thread_size.x + local_size.x - 1) / local_size.x,
        (thread_size.y + local_size.y - 1) / local_size.y,
        (thread_size.z + local_size.z - 1) / local_size.z
    );
}
Renderer::~Renderer() {
    if (m_device)
        m_device->WaitIdle();
}
