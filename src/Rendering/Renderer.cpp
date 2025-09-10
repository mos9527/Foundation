// ReSharper disable CppMemberFunctionMayBeConst
#include <array>
#include <ranges>
#include <algorithm>
#include <filesystem>
#include <fstream>

#include <Math/Math.hpp>
#include <RHICore/Device.hpp>

#include "Renderer.hpp"
#include "ShaderReflection.hpp"
#include "spdlog/fmt/bundled/compile.h"

using namespace Foundation::Core;
using namespace Foundation::Rendering;
// Semaphore counter
#define SEM_COUNTER(ord) (m_frame + ord + 1LL)
#define SEM_COUNTER_PREV(ord) (m_frame + ord)
const char* kShaderDescriptorFirstBindingErrorHelp = "This can be caused by one of the following:\n"
"   - Parameter is optimized-out, and the binding is kept as is.\n"
"   - Multiple entrypoints in the same shader, but they don't access the same parameters.\n"
"Tips:\n"
"   Try separating the entrypoints into different shader files, or sort the binding declarations"
"so that the used bindings are continuous from 0.";
const char* kShaderDescriptorFirstSetErrorHelp = kShaderDescriptorFirstBindingErrorHelp;
constexpr size_t kExecuteArenaSize = 16 * (1 << 20); // 16 MiB for render graph execution
const RHIPipelineStage kComputeStagesMask =
      RHIPipelineStageBits::FragmentShader |
      RHIPipelineStageBits::VertexShader |
      RHIPipelineStageBits::MeshShader |
      RHIPipelineStageBits::RayTracingShader |
      RHIPipelineStageBits::AllGraphics;
Renderer::Renderer(RendererDesc const& desc, RHIApplicationObjectHandle<RHIDevice> device, RHIDeviceObjectHandle<RHISwapchain> swapchain, Allocator* allocator)
    : m_state(State::Undefined), m_allocator(allocator), m_desc(desc), m_swaps(m_allocator),
      m_device(device), m_executeArena(m_allocator, kExecuteArenaSize), m_executeAlloc(m_executeArena),
      m_waitIdle(device.Get()) {
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
    BeginSetup();
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
    CHECK_MSG(m_state == State::Undefined || m_state == State::PostSetup, "BeginSetup() is called at construction time - you shouldn't call this again. Current state is {}", m_state);
    m_setup = ConstructUnique<Setup>(m_allocator, m_allocator);
    m_state = State::Setup;
}
ResourceHandle Renderer::CreateTextureView(
    PassHandle pass, ResourceHandle handle, RHITextureViewDesc const& desc) const
{
    CHECK(m_state == State::Setup);
    m_setup->trackedViews.emplace_back(handle, desc);
    ResourceHandle hdl = m_setup->trackedViews.size() - 1;
    m_setup->trackedPasses[pass].texviews.emplace_back(hdl);
    return hdl;
}
ResourceHandle Renderer::CreateSampler(RHIDeviceSampler::SamplerDesc const& desc) const
{
    CHECK(m_state == State::Setup);
    m_setup->trackedSamplers.emplace_back(desc);
    return m_setup->trackedSamplers.size() - 1;
}
void Renderer::DeclareBufferAccess(PassHandle pass, ResourceHandle handle, RHIPipelineStage stage, RHIResourceAccess access) const
{
    CHECK(m_state == State::Setup);
    auto& resource = m_setup->trackedResources[handle];
    // Check for overlap
    for (auto const& [h, _access, _stage] : m_setup->trackedPasses[pass].bufferUsages)
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
    PassHandle pass, ResourceHandle handle, RHIPipelineStage stage, RHITextureSubresourceRange range, RHIResourceAccess access, RHITextureLayout layout) const
{
    CHECK(m_state == State::Setup);
    auto& resource = m_setup->trackedResources[handle];
    // Check for overlap    
    auto [mip_begin, mip_end] = range.GetMipLevelRange();
    auto [layer_begin, layer_end] = range.GetArrayLayerRange();
    for (auto const& [h, _access, _stage, r, _layout] : m_setup->trackedPasses[pass].textureUsages) {
        if (h == handle) {
            auto [r_mip_begin, r_mip_end] = r.GetMipLevelRange();
            auto [r_layer_begin, r_layer_end] = r.GetArrayLayerRange();
            // Mip intersects
            if (!(mip_end < r_mip_begin || mip_begin > r_mip_end)) {
                // Layer intersects
                if (!(layer_end < r_layer_begin || layer_begin > r_layer_end))
                    throw std::runtime_error("Overlap detected. Texture access must be disjoint.");
            }
        }
    }
    // Do this for all sub resources in range
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
    StringView entry_point,
    std::filesystem::path const& shader_path
) const
{
    CHECK(m_state == State::Setup);
    CHECK_MSG(stage.is_bitmask(), "Only one stage can be bound to a shader per pass");
    for (auto const& [path, ep, st] : m_setup->trackedPasses[pass].shaders)
        if (st & stage)
            throw std::runtime_error("Some previous shader stage(s) already bound to a shader");
    m_setup->trackedPasses[pass].shaders.emplace_back(shader_path, entry_point, stage);
}
void Renderer::BindVertexInput(
    PassHandle pass,
    RHIPipelineState::PipelineStateDesc::VertexInput const& info
) const
{
    CHECK(m_state == State::Setup);
    m_setup->trackedPasses[pass].vertex_input_bindings.insert(
        m_setup->trackedPasses[pass].vertex_input_bindings.end(),info.bindings.begin(), info.bindings.end()
    );
    m_setup->trackedPasses[pass].vertex_input_attributes.insert(
        m_setup->trackedPasses[pass].vertex_input_attributes.end(),info.attributes.begin(), info.attributes.end()
    );
}
void Renderer::BindPushConstant(
    PassHandle pass, RHIShaderStage stage,
    size_t offset, size_t size
) const
{
    CHECK(m_state == State::Setup);
    for (auto const& [s, _offset, _size] : m_setup->trackedPasses[pass].push_constants)
        if (s & stage)
            throw std::runtime_error("Some previous shader stage(s) already has Push Constants ranges");
    m_setup->trackedPasses[pass].push_constants.emplace_back(stage, offset, size);
}
void Renderer::BindBufferUniform(
    PassHandle pass, ResourceHandle buffer,
    RHIPipelineStage stage, StringView bind_point
) const
{
    CHECK(m_state == State::Setup);
    DeclareBufferAccess(pass, buffer, stage,
        RHIResourceAccessBits::UniformRead
    );
    m_setup->trackedPasses[pass].buf_bindings.emplace_back(buffer, RHIDescriptorType::UniformBuffer, bind_point);
    m_setup->binding_counts[RHIDescriptorType::UniformBuffer]++;
}
void Renderer::BindBufferStorage(
    PassHandle pass, ResourceHandle buffer,
    RHIPipelineStage stage, StringView bind_point
) const
{
    CHECK(m_state == State::Setup);
    DeclareBufferAccess(pass, buffer, stage,
        RHIResourceAccessBits::ShaderRead
    );
    m_setup->trackedPasses[pass].buf_bindings.emplace_back(buffer, RHIDescriptorType::StorageBuffer, bind_point);
    m_setup->binding_counts[RHIDescriptorType::StorageBuffer]++;
}
void Renderer::BindBufferUnordered(
    PassHandle pass, ResourceHandle buffer,
    RHIPipelineStage stage, StringView bind_point
) const
{
    CHECK(m_state == State::Setup);
    DeclareBufferAccess(pass, buffer, stage,
        RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite
    );
    m_setup->trackedPasses[pass].buf_bindings.emplace_back(buffer, RHIDescriptorType::StorageBuffer, bind_point);
    m_setup->binding_counts[RHIDescriptorType::StorageBuffer]++;
}
void Renderer::BindBufferShaderRead(PassHandle pass, ResourceHandle buffer, RHIPipelineStage stage) const
{
    CHECK(m_state == State::Setup);
    DeclareBufferAccess(pass, buffer, stage,
        RHIResourceAccessBits::ShaderRead
    );
}
void Renderer::BindBufferCopyDst(PassHandle pass, ResourceHandle buffer) const
{
    CHECK(m_state == State::Setup);
    DeclareBufferAccess(pass, buffer,
        RHIPipelineStageBits::Transfer,
        RHIResourceAccessBits::TransferWrite
    );
}
void Renderer::BindBufferCopySrc(PassHandle pass, ResourceHandle buffer) const
{
    CHECK(m_state == State::Setup);
    DeclareBufferAccess(pass, buffer,
        RHIPipelineStageBits::Transfer,
        RHIResourceAccessBits::TransferRead
    );
}
void Renderer::BindTextureSampler(
    PassHandle pass, ResourceHandle sampler,
    StringView shader_name
) const
{
    CHECK(m_state == State::Setup);
    m_setup->trackedPasses[pass].samplers.emplace_back(sampler, shader_name);
    m_setup->binding_counts[RHIDescriptorType::Sampler]++;
}
ResourceHandle Renderer::BindTextureSRV(
    PassHandle pass, ResourceHandle texture,
    StringView shader_name, RHIPipelineStage stage,
    RHITextureViewDesc const& desc
) const
{
    CHECK(m_state == State::Setup);
    DeclareTextureAccess(pass, texture, stage, desc.range,
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
    StringView shader_name, RHIPipelineStage stage,
    RHITextureViewDesc const& desc
) const
{
    CHECK(m_state == State::Setup);
    DeclareTextureAccess(pass, texture, stage, desc.range,
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
) const
{
    CHECK(m_state == State::Setup);
    auto& tpass = m_setup->trackedPasses[pass];
    CHECK_MSG(tpass.queue == RHIDeviceQueueType::Graphics, "RTV (Render Target Views) are only supported on Graphics queues");
    RHITextureAspectFlag kRTVBits = RHITextureAccessFlagBits::Color;
    CHECK_MSG((desc.range.layer.access | kRTVBits == kRTVBits) && (desc.range.layer.access & kRTVBits),
        "RTV view must have exactly one layer, and the access flag must be Color.");
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
) const
{
    CHECK(m_state == State::Setup);
    auto& tpass = m_setup->trackedPasses[pass];
    CHECK_MSG(tpass.queue == RHIDeviceQueueType::Graphics, "DSV (Depth Stencil Views) are only supported on Graphics queues");
    RHITextureAspectFlag kDSVBits = RHITextureAccessFlagBits::Depth | RHITextureAccessFlagBits::Stencil;
    CHECK_MSG((desc.range.layer.access | kDSVBits == kDSVBits) && (desc.range.layer.access & kDSVBits),
        "DSV view must have exactly one layer, and the access flag must be Depth and/or Stencil.");
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
void Renderer::BindBackbufferRTV(PassHandle pass) const
{
    CHECK(m_state == State::Setup);
    auto& tpass = m_setup->trackedPasses[pass];
    CHECK_MSG(tpass.queue == RHIDeviceQueueType::Graphics, "RTV (Render Target Views) are only supported on Graphics queues");
    tpass.write_backbuffer = true;
}
void Renderer::BindTextureCopyDst(
    PassHandle pass, ResourceHandle texture,
    RHITextureSubresourceRange const& range
) const
{
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
) const
{
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
    if (!m_setup->trackedPasses.empty()) {
        // Setup all passes
        for (auto& pass : m_setup->trackedPasses) {
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
void Renderer::CullPasses(PassHandle epilogue) const
{
    CHECK(m_state == State::Setup);
    CHECK(epilogue < m_setup->trackedPasses.size());
    // Cull and topsort
    Vector<PassHandle>
        topo(m_allocator),
        vis(m_setup->trackedPasses.size(), m_allocator),
        depth(m_setup->trackedPasses.size(), m_allocator); // Depth in graph from epilogue
    topo.reserve(m_setup->trackedPasses.size());
    auto topsort = [&](PassHandle u, PassHandle pa, auto&& dfs) -> void {
        vis[u] = 1;
        for (const auto& v : m_setup->graph[u] | std::views::keys) {
            depth[v] = std::max(depth[u] + 1, depth[v]);
            if (vis[v] == 1)
                throw std::runtime_error("Cycle detected in render graph");
            if (vis[v] == 0) dfs(v, u, dfs);
        }
        vis[u] = 2;
        m_setup->trackedPasses[u].used = true;
        topo.push_back(u);
        };
    if (!m_setup->graph.empty()) {
        topsort(epilogue, -1, topsort);
        m_setup->execution = topo;
    }
    else {
        // No dependency from any passes
        // Execute only the epilogue
        m_setup->execution.push_back(epilogue);
        m_setup->trackedPasses[epilogue].used = true;
    }
    m_setup->epilogue = epilogue;
    // Collect active resources
    auto& exec = m_setup->execution;
    for (PassHandle ord = 0; ord < exec.size(); ord++) {
        auto& pass = m_setup->trackedPasses[exec[ord]];
        // Derive lifetimes for resources from execution order
        // FinalizeResources() uses this to overlap resources.
        pass.ord = ord, pass.depth = depth[pass.handle];
        auto& resources = pass.resources;
        // Sort then make unique
        std::ranges::sort(resources);
        resources.erase(std::ranges::unique(resources).begin(), resources.end());
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
    // Reorder passes within the same depth level to their relative insertion order (i.e. handle values)
    // Topo order is preserved
    for (PassHandle i = 0, j = 0; i < exec.size();i = j) {
        while (j < exec.size() && m_setup->trackedPasses[exec[j]].depth == m_setup->trackedPasses[exec[i]].depth)
            j++;
        std::ranges::sort(exec.begin() + i, exec.begin() + j, [&](PassHandle a, PassHandle b) {
            return m_setup->trackedPasses[a].handle < m_setup->trackedPasses[b].handle;
        });
    }
    LOG_RUNTIME(Renderer, debug, "** Render Graph GraphViz **\n{}", DbgDumpGraphviz());
    LOG_RUNTIME(Renderer, debug, "** Render Graph Execution Order **\n{}", DbgDumpActivePasses());
}
void Renderer::BuildPipelineState(PassHandle pass) {
    auto& tracked = m_setup->trackedPasses[pass];
    Vector<RHIPipelineState::PipelineStateDesc::ShaderStage> pso_stages(m_allocator);
    // Load shader bytecode
    if (tracked.shaders.empty())
        return; // Pass with no shaders
    LOG_RUNTIME(Renderer, debug, "** Building PSO for {} [{}] **", tracked.name, pass);
    Vector<char> data(m_allocator);
    Map<std::filesystem::path, RHIDeviceScopedObjectHandle<RHIShaderModule>> shaders(m_allocator);
    Map<std::filesystem::path, UniquePtr<ShaderReflection>> reflections(m_allocator);
    for (auto const& [shader_path, entry_point, stage] : tracked.shaders) {
        if (!shaders.contains(shader_path)) {
            LOG_RUNTIME(Renderer, debug, "Loading shader {}", shader_path.string());
            std::ifstream file(shader_path, std::ios::binary);
            CHECK_MSG(file.good(), "Failed to open shader file {}", shader_path.string());
            data.resize(std::filesystem::file_size(shader_path));
            file.read(data.data(), static_cast<uint32_t>(data.size()));
            CHECK_MSG(file.gcount() == data.size(), "Shader read failure. Read {} bytes, expected {}", file.gcount(), data.size());
            // Verify shader stage
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
        CHECK_MSG(tracked.rtvs.empty() && tracked.dsv == kInvalidHandle, "Pass {} uses Compute Shader, and cannot have RTVs or DSVs.", tracked.name);
    }
    // Check variable bindings to be consistent across stages
    // [name, [set, binding]]
    Map<String, Pair<uint32_t, uint32_t>> var_bind_points(m_allocator);
    // Check if any shader in the pipeline uses PC
    for (auto const& [path, refl] : reflections){
        if (!refl->m_pushConstants.empty())
        {
            CHECK_MSG(refl->m_pushConstants.size() == 1, "Shader uses more than Push Constant block. This is not accepted by most drivers.");
            CHECK_MSG(!tracked.push_constants.empty(), "Pass does not declare Push Constant ranges, but shader {} uses them.", path.string());
        }
        for (auto& bind : refl->m_bindings) {
            CHECK_MSG(
                !bind.name.empty(),
                "Unnamed bindings are not supported. Enable debug information for shader {}",
                path.string()
            );
            auto it = var_bind_points.find(bind.name);
            if (it == var_bind_points.end())
                var_bind_points[bind.name] = { bind.descriptorSet, bind.binding };
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
    Map<String, RHIDescriptorType> var_types(m_allocator);
    Map<String, ResourceHandle> var_handles(m_allocator);
    Map<String, ResourceHandle> var_samplers(m_allocator);
    for (auto& [vhdl, dtype, binding] : tracked.tex_bindings) {
        auto it = var_types.find(binding);
        if (it == var_types.end())
            var_types[binding] = dtype, var_handles[binding] = vhdl;
        else {
            auto& dtype_prev = it->second;
            auto& vhdl_prev = var_handles[binding];
            CHECK(dtype_prev == dtype && vhdl_prev == vhdl);
        }
    }
    for (auto& [rhdl, dtype, binding] : tracked.buf_bindings) {
        auto it = var_types.find(binding);
        if (it == var_types.end())
            var_types[binding] = dtype, var_handles[binding] = rhdl;
        else {
            auto& dtype_prev = it->second;
            auto& rhdl_prev = var_handles[binding];
            CHECK(dtype_prev == dtype && rhdl_prev == rhdl);
        }
    }
    for (auto& [sampler_handel, binding] : tracked.samplers) {
        auto it = var_types.find(binding);
        if (it == var_types.end())
            var_types[binding] = RHIDescriptorType::Sampler, var_samplers[binding] = sampler_handel;
        else {
            auto& dtype_prev = it->second;
            auto& sampler_handle = var_samplers[binding];
            CHECK(dtype_prev == RHIDescriptorType::Sampler && sampler_handle == sampler_handle);
        }
    }
    if (!var_bind_points.empty()) {
        LOG_RUNTIME(Renderer, debug, "Pipeline Parameters");
        for (auto& [name, dtype] : var_types) {
            if (!var_bind_points.contains(name))
                continue;
            auto [set, binding] = var_bind_points[name];
            LOG_RUNTIME(Renderer, debug, "\t{}: set {}, binding {}, type {}", name, set, binding, dtype);
        }
    }
    // [[set, binding], name]
    Vector<Pair<Pair<uint32_t, uint32_t>, String>> bindings(m_allocator);
    bindings.reserve(var_types.size());
    for (auto& [name, bind] : var_bind_points)
        bindings.emplace_back( bind, name );
    std::ranges::sort(bindings);
    // Separate into descriptor sets
    Vector<RHIDeviceDescriptorSetLayoutDesc::Binding> set_bindings(m_allocator);
    for (const auto& binding : bindings | std::views::values) {
        // !! TODO: Descriptor Arrays
        CHECK_MSG(var_types.contains(binding), "Binding {} is not bound by pass {}, but is used by one of its shaders.", binding, tracked.name);
        set_bindings.push_back({ .count = 1, .stage = RHIShaderStageBits::All, .type = var_types[binding] });
    }
    // Check if our first set is not 0
    if (!bindings.empty() && !bindings[0].first.first == 0)
    {
        LOG_RUNTIME(BuildPipelineState, err,
            "Binding set numbers must start from 0. Error at set {} binding {} in pass {}.",
            bindings[0].first.first, bindings[0].first.second, tracked.name
        );
        LOG_RUNTIME(BuildPipelineState, info, kShaderDescriptorFirstSetErrorHelp);
        CHECK_MSG(false, "Binding set numbers must start from 0.");
    }
    for (uint32_t i = 0, j = 0; i < bindings.size(); i = j) {
        uint32_t set = bindings[i].first.first;
        // Check if our first binding is not 0
        if (!bindings[i].first.second == 0)
        {
            LOG_RUNTIME(BuildPipelineState, err,
                "Binding numbers must start from 0 in each descriptor set. Error at set {} binding {} in pass {}.",
                set, bindings[i].first.second, tracked.name
            );
            LOG_RUNTIME(BuildPipelineState, info, kShaderDescriptorFirstBindingErrorHelp);
            CHECK_MSG(false, "Binding binding numbers must start from 0.");
        }
        while (j < bindings.size() && bindings[j].first.first == set)
            j++;
        // Create descriptor set layout
        tracked.desc_layouts.push_back(m_device->CreateDescriptorSetLayout(
            { .bindings = { set_bindings.cbegin() + i, set_bindings.cbegin() + j} }
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
            auto const& [bind, name] = bindings[k];
            auto const& [set, binding] = bind;
            auto const& hdl = var_handles[name];
            CHECK_MSG(var_types.contains(name), "Binding {} is undefined in pass {}, but referenced by one of its shaders", name, tracked.name);
            auto const& type = var_types[name];
            using enum RHIDescriptorType;
            switch (type)
            {
            case Sampler:
                {
                    CHECK_MSG(var_samplers.contains(name), "Shader expects a Sampler at {}, but it's not bound by pass {}", name, tracked.name);
                    auto& sampler_handle = var_samplers[name];
                    auto* sampler = DerefSampler(sampler_handle);
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
        .vertex_input = {
            .bindings = tracked.vertex_input_bindings,
            .attributes = tracked.vertex_input_attributes
        },
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
    Vector<RHIPipelineState::PipelineStateDesc::Attachment> attachments(m_allocator);
    if (tracked.write_backbuffer) {
        CHECK_MSG(tracked.rtvs.empty(), "Pass {} writes to backbuffer, and cannot have other RTVs.", tracked.name);
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
        auto& [dsv_handle, desc] = m_setup->trackedViews[tracked.dsv];
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
    if (!m_setup->binding_counts.empty()) {
        Vector<RHIDeviceDescriptorPool::PoolDesc::Binding> bindings(m_allocator);
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
    // Optionally used to be actually waited on (inter-queue, etc.), but will always be signaled
    for (PassHandle ord = 0; ord < m_setup->execution.size(); ord++) {
        auto& pass = m_setup->trackedPasses[m_setup->execution[ord]];
        if (m_desc.async /* && pass.has_cross_queue_dependent */) {
            // TODO: Figure out how to accurately detect dependency across *frames*
            // Creating one for all passes works for now.
            pass.asyncSemaphore = m_device->CreateSemaphore(true);
            pass.asyncSemaphore->DebugSetObjectName(
                fmt::format("Timeline Semaphore of {} [{}]", pass.name, pass.handle).c_str()
            );
        }
    }
    // !! TODO: Overlap transient resources to with non-overlapping lifetimes with aliasing
    for (const auto& handle : m_setup->activeResources | std::views::keys) {
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
            [&](RHIDeviceObjectHandle<RHITexture> const& hdl) { m_resources->resources[handle] = hdl; },
            [&](RHIBuffer* const ptr)
            {
                m_resources->resources[handle] = ptr;
            },
            [&](RHITexture* const ptr)
            {
                m_resources->resources[handle] = ptr;
            },
            [&](auto const&) { throw std::runtime_error("Unhandled resource type at creation time"); }
        );
    }
    // Add back buffers (if any)
    if (m_desc.present)
    {
        for (size_t i = 0; i < m_frameSwaps;i++)
        {
            ResourceHandle handle = m_swaps[i].rt_handle;
            auto& tres = m_setup->trackedResources[handle];
            m_resources->resources[handle] = tres.desc.Get<RHITexture*>();
        }
    }
    // Create texture views
    Vector<ResourceHandle> activeViews(m_allocator), activeSamplers(m_allocator);
    for (PassHandle ord = 0; ord < m_setup->execution.size(); ord++) {
        auto& pass = m_setup->trackedPasses[m_setup->execution[ord]];
        for (auto hdl : pass.texviews)
            activeViews.push_back(hdl);
        for (const auto& key : pass.samplers | std::views::keys)
            activeSamplers.push_back(key);
    }
    // Instantiate views
    std::ranges::sort(activeViews);
    activeViews.erase(std::ranges::unique(activeViews).begin(), activeViews.end());
    m_resources->fit(activeViews.size());
    for (auto hdl : activeViews) {
        auto [rhdl, desc] = m_setup->trackedViews[hdl];
        auto res = DerefResource(rhdl).Get<RHITexture*>();
        m_resources->views[hdl] = res->CreateTextureView(desc);
    }
    // Instantiate samplers
    std::ranges::sort(activeSamplers);
    activeSamplers.erase(std::ranges::unique(activeSamplers).begin(), activeSamplers.end());
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
#pragma endregion
void Renderer::SetFrameSyncObjects() {
    while (m_swaps.size() < m_frameSwaps)
        m_swaps.emplace_back(m_allocator);
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
        // If changing swapchain during execution (e.g. due to resize exception)
        // Wait for GPU to be idle
        m_device->WaitIdle();
        m_state = State::PostSetup;
    }
    SetFrameSyncObjects();
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
    // Reset semaphores index
    m_currentSync = 0;
}
RHIPipelineStage Renderer::ExecuteGetPassAllCurrentStages(TrackedPass& pass)
{
    RHIPipelineStage ans{};
    for (auto [hdl, access, stage, range, layout] : pass.textureUsages)
    {
        auto& tres = m_setup->trackedResources[hdl];
        for (auto& sta : tres.GetLastSubresourceStateOf(range))
            ans |= sta.stage;
    }
    if (pass.write_backbuffer)
        ans |= RHIPipelineStageBits::ColorAttachmentOutput;
    for (auto [hdl, access, stage] : pass.bufferUsages)
    {
        auto& tres = m_setup->trackedResources[hdl];
        ans |= tres.lastBufferState.stage;
    }
    return ans;
}
void Renderer::ExecuteBarrierSubresource(TrackedResource& tres, RHITextureSubresourceRange const& range,RHIResourceAccess access, RHIPipelineStage stage, RHITextureLayout layout, RHICommandList* cmd)
{
    RHITexture* res = DerefResource(tres.handle).Get<RHITexture*>();
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
void Renderer::ExecuteBarrierBuffer(TrackedResource& tres, RHIResourceAccess access, RHIPipelineStage stage, RHICommandList* cmd)
{
    RHIBuffer* res = DerefResource(tres.handle).Get<RHIBuffer*>();
    if (tres.lastBufferState.access == access && tres.lastBufferState.stage == stage)
        return;
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
void Renderer::ExecuteBarriers(TrackedPass& pass, RHICommandList* cmd)
{
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
        ExecuteBarrierSubresource(tres, range, access, stage, layout, cmd);
    }
    // Backbuffer
    // A special case with known usages.
    // We never create resource per-swap so tracking Back buffers by passes
    // is not possible. The BB is also opaque to the passes for the same reasons.
    // Synchronization for other resources are already handled above,
    // and should eliminate any redundant per-pass resource creation.
    if (pass.write_backbuffer)
    {
        const RHIResourceAccess rt_access = RHIResourceAccessBits::RenderTargetWrite;
        const RHITextureLayout rt_layout = RHITextureLayout::RenderTarget;
        const RHIPipelineStage rt_stage = RHIPipelineStageBits::ColorAttachmentOutput;
        auto& tres = m_setup->trackedResources[m_swaps[m_currentSwap].rt_handle];
        ExecuteBarrierSubresource(tres, {}, rt_access, rt_stage, rt_layout, cmd);
    }
    // Buffers
    // These are always global i.e. at most one per buffer per pass.
    for (auto [hdl, access, stage] : pass.bufferUsages) {
        auto& tres = m_setup->trackedResources[hdl];
        ExecuteBarrierBuffer(tres, access, stage, cmd);
    }
    cmd->EndTransition();
}
bool Renderer::ExecuteSubmitOrContinue(
    TrackedPass& pass, RHICommandList* cmd,
    const RHIDeviceQueue* queue,
    bool final_submit,
    Span<const Tuple<RHIDeviceSemaphore*, RHIPipelineStage, size_t>> extra_waits
)
{
    CHECK(m_state == State::Execute && m_executeAlloc);
    Vector<Pair<RHIDeviceSemaphore*, size_t>> waits(m_executeAlloc.Ptr()), signals(m_executeAlloc.Ptr());
    Vector<RHIPipelineStage> waits_stages(m_executeAlloc.Ptr());
    for (auto const& [sem, stage, counter] : extra_waits) {
        waits.emplace_back(sem, counter);
        waits_stages.push_back(stage);
    }
    // Collect waited semaphores from inter-queue dependencies
    if (m_desc.async)
    {
        signals.emplace_back(pass.asyncSemaphore.Get(), SEM_COUNTER(pass.ord));
        // Pass that depends on a producer pass that's on another queue
        // needs external synchronization.
        auto check_wait = [&](PassHandle other) -> bool {
            if (other != kInvalidHandle) {
                auto& opass = m_setup->trackedPasses[other];
                if (opass.queue != pass.queue) {
                    CHECK_MSG(opass.asyncSemaphore.IsValid(), "FIXME-Async Compute: Pass {} [{}] is not valid to be waited on", opass.name, other);
                    // Wait on the producer pass's semaphore
                    if (opass.pass->IsSkipped(opass.handle, this))
                        return false; // Skip waiting on skipped passes
                    if (opass.ord > pass.ord)
                    {
                        // From the previous frame
                        waits.emplace_back(opass.asyncSemaphore.Get(), SEM_COUNTER_PREV(opass.ord));
                        return true;
                    } else
                    {
                        waits.emplace_back(opass.asyncSemaphore.Get(), SEM_COUNTER(opass.ord));
                        return true;
                    }
                }
            }
            return false;
        };
        // Textures
        for (auto const& [hdl, access, stage, range, layout] : pass.textureUsages) {
            auto& res = m_setup->trackedResources[hdl];
            for (auto& sta : res.GetLastSubresourceStateOf(range)) {
                if (sta.producer == pass.handle)
                    continue;
                if (check_wait(sta.producer))
                    waits_stages.emplace_back(stage);
                if (access & kAllShaderWrites)
                    sta.producer = pass.handle;
            }
        }
        // Buffer ranges
        for (auto const& [hdl, access, stage] : pass.bufferUsages) {
            auto& res = m_setup->trackedResources[hdl];
            if (res.lastBufferState.producer == pass.handle)
                continue;
            if (check_wait(res.lastBufferState.producer))
                waits_stages.emplace_back(stage);
            if (access & kAllShaderWrites)
                res.lastBufferState.producer = pass.handle;
        }
    }
    // Submit or continue on the same list
    if (final_submit)
    {
        // Always submit the last one, and present if needed
        if (m_desc.present)
        {
            CHECK_MSG(queue == m_gfxQueue, "Last pass must be on the Graphics queue to present!");
            cmd->BeginTransition();
            ExecuteBarrierSubresource(
                m_setup->trackedResources[m_swaps[m_currentSwap].rt_handle],
                {},
                {},
                RHIPipelineStageBits::BottomOfPipe,
                RHITextureLayout::Present,
                cmd
            );
            cmd->EndTransition();
            cmd->End();
            waits_stages.push_back(RHIPipelineStageBits::ColorAttachmentOutput);
            queue->Submit({
                .timeline_waits = waits,
                .timeline_signals = signals,
                .waits = {{ m_swaps[m_currentSync].present.Get() }},
                .waits_stages = waits_stages,
                .signals = {{ m_swaps[m_currentSwap].render.Get() }},
                .cmd_lists = {{ cmd }},
                .fence = m_swaps[m_currentSync].fence.Get()
            });
            queue->Present({
                .image_index = m_currentSwap,
                .swapchain = m_swapchain.Get(),
                .waits = {{ m_swaps[m_currentSwap].render.Get() }}
            });
        } else
        {
            cmd->End();
            queue->Submit({
                .timeline_waits = waits,
                .timeline_signals = signals,
                .waits_stages = waits_stages,
                .cmd_lists = { cmd },
            });
        }
        return true;
    } else
    {
        if (!m_desc.async) return false; // Continue
        if (waits.empty() && signals.empty()) return false; // Continue
        // Otherwise submit now for subsequent passes to wait on
        cmd->End();
        queue->Submit({
            .timeline_waits = waits,
            .timeline_signals = signals,
            .waits_stages = waits_stages,
            .cmd_lists = { cmd },
        });
        return true;
    }
}
/**
 * Hot path. States are pre-allocated, and all auxiliary allocations are
 * done on the stack e.g. command list recording, runtime (IsSkipped()) pass culling.
 */
void Renderer::Execute() {
    CHECK_MSG(m_state == State::PostSetup, "Renderer bad state ({}). Did you call EndSetup()?", m_state);
    m_executeAlloc.Reset(m_executeArena);
    m_state = State::Execute;
    // Execute passes
    auto& passes = m_setup->trackedPasses;
    m_device->WaitForFences({ m_swaps[m_currentSync].fence }, true, -1);
    if (m_desc.present)
        m_currentSwap = m_swapchain->GetNextImage(-1, m_swaps[m_currentSync].present, {});
    m_device->ResetFences({ m_swaps[m_currentSync].fence });
    // Async compute supporting command lists
    int cmd_index = 0, cmd_comp_index = 0;
    RHIDeviceQueue* queue = m_gfxQueue;
    RHICommandList* cmd = m_swaps[m_currentSync].cmd_at(cmd_index, m_cmdPool.Get());
    auto set_next_graphics = [&]() {
        CHECK_MSG(cmd_index < kMaxCommandListsPerSwap, "FIXME-Async Compute: All transient Graphics Command lists exhausted");
        queue = m_gfxQueue, cmd = m_swaps[m_currentSync].cmd_at(cmd_index++, m_cmdPool.Get());
        cmd->Reset(), cmd->Begin();
    };
    auto set_next_compute = [&]() {
        CHECK_MSG(cmd_comp_index < kMaxCommandListsPerSwap, "FIXME-Async Compute: All transient Compute Command lists exhausted");
        queue = m_compQueue, cmd = m_swaps[m_currentSync].comp_cmds_at(cmd_comp_index++, m_compCmdPool.Get());
        cmd->Reset(), cmd->Begin();
    };
    auto set_next_pass_queue = [&](const TrackedPass& pass) {
        switch (pass.queue)
        {
        case RHIDeviceQueueType::Graphics:
            set_next_graphics();
            break;
        case RHIDeviceQueueType::Compute:
            set_next_compute();
            break;
        default:
            throw std::runtime_error("Unsupported queue type");
            break;
        }
    };
    // Take non-skipped passes only
    auto execution_view = std::views::all(m_setup->execution)
        | std::views::filter([&](PassHandle hdl) { return !passes[hdl].pass->IsSkipped(hdl, this); });
    auto execution = Vector<PassHandle>(execution_view.begin(), execution_view.end(), m_executeAlloc.Ptr());
    size_t pass_cnt = execution.size();
    if (!execution.empty())
        set_next_pass_queue(passes[*execution.begin()]);
    Vector<Tuple<RHIDeviceSemaphore*, RHIPipelineStage, size_t>> extra_wait(m_executeAlloc.Ptr());
    for (auto i = 0; i < pass_cnt; ++i) {
        auto& pass = passes[execution[i]];
        if (pass.queue == RHIDeviceQueueType::Compute) {
            // XXX: Suboptimal. We can't stay on compute to transition if the pass
            // uses resources that has these flags.
            if (auto stages = ExecuteGetPassAllCurrentStages(pass); stages & kComputeStagesMask) {
                set_next_graphics();
                cmd->DebugInsertMarker("<Sync with Graphics Resources>");
                ExecuteBarriers(pass, cmd);
                cmd->End();
                auto* extra_sem = m_swaps[m_currentSync].barrier_semaphore_at(cmd_index - 1, m_device.Get());
                auto extra_ctr = SEM_COUNTER(cmd_index - 1);
                extra_wait.emplace_back( extra_sem, pass.GetMaxPipelineStages(), extra_ctr );
                queue->Submit({
                    .timeline_signals = {{{extra_sem, extra_ctr}}},
                    .cmd_lists = { cmd },
                });
                cmd_comp_index--;
                set_next_compute();
            } else {
                ExecuteBarriers(pass, cmd);
            }
        }
        else {
            ExecuteBarriers(pass, cmd);
        }
        cmd->DebugBegin(pass.name.c_str());
        pass.pass->Record(pass.handle, this, cmd);
        cmd->DebugEnd();
        // Submit if needed
        bool final_submit = i == pass_cnt - 1;
        bool submitted = ExecuteSubmitOrContinue(pass, cmd, queue, final_submit, extra_wait);
        if (!extra_wait.empty())
        {
            CHECK_MSG(submitted, "FIXME-Async Compute: Extra wait not consumed");
            extra_wait.clear();
        }
        if (submitted && !final_submit)
            set_next_pass_queue(passes[execution[i + 1]]);
    }
    m_currentSync = (m_currentSync + 1) % m_frameSwaps;
    m_frame++;
    m_state = State::PostSetup;
}
void Renderer::CmdSetPipeline(PassHandle pass, RHICommandList* cmd) const
{
    CHECK(m_state == State::Execute);
    auto& tpass = m_setup->trackedPasses[pass];
    CHECK_MSG(tpass.pso.IsValid(), "Current pass has no Pipeline state.");
    cmd->SetPipeline({
        .pipeline = tpass.pso.Get(),
        .type = tpass.compute_pass ? RHIDevicePipelineType::Compute : RHIDevicePipelineType::Graphics
    });
    if (!tpass.p_desc_sets.empty())
        cmd->BindDescriptorSet(
            tpass.compute_pass ? RHIDevicePipelineType::Compute : RHIDevicePipelineType::Graphics,
            tpass.pso.Get(),
            tpass.p_desc_sets
        );
}
void Renderer::CmdBeginGraphics(PassHandle pass, RHICommandList* cmd,
    RHIExtent2D const& extent,
    Optional<RHIClearColor> const&  clear_rtv,
    Optional<RHIClearDepthStencil> const& clear_dsv
) {
    CHECK(m_state == State::Execute);
    auto& tpass = m_setup->trackedPasses[pass];
    CHECK_MSG(tpass.pso.IsValid(), "Current pass has no Pipeline state.");
    Vector<RHICommandList::GraphicsDesc::Attachment> rtvs(m_executeAlloc.Ptr());
    if (tpass.write_backbuffer) {
        const RHIExtent2D backbuffer = GetSwapchainExtent();
        CHECK_MSG(
            extent.x <= backbuffer.x && extent.y <= backbuffer.y,
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
                "Graphics extent too large for Render Target on {}", tres.name
            );
            rtvs.push_back({
                .image_view = DerefTextureView(rtv),
                .clear_color = clear_rtv
            });
        }
    }
    if (tpass.dsv != kInvalidHandle) {
        auto& [depth_hdl, desc] = m_setup->trackedViews[tpass.dsv];
        auto const& tres = m_setup->trackedResources[depth_hdl];
        RHITexture* res = DerefResource(depth_hdl).Get<RHITexture*>();
        CHECK_MSG(
            res->m_desc.extent.x >= extent.x && res->m_desc.extent.y >= extent.y,
            "Graphics extent too large for Depth buffer {}",
            tres.name
        );
        cmd->BeginGraphics({
            .color_attachments = rtvs,
            .depth_attachment = {
                .image_view = DerefTextureView(tpass.dsv),
                .image_layout = RHITextureLayout::DepthStencil,
                .clear_depth_stencil = clear_dsv
            },
            .width = extent.x,
            .height = extent.y
        });
    }
    else {
        CHECK_MSG(
            !rtvs.empty(), "No RTVs or DSV bound for graphics pass {} [{}]", tpass.name, pass
        )
        cmd->BeginGraphics({
            .color_attachments = rtvs,            
            .width = extent.x,
            .height = extent.y
        });
    }
}
RHIExtent3D Renderer::CmdGetComputeLocalSize(const PassHandle pass) const
{
    CHECK(m_state == State::Execute);
    auto& tpass = m_setup->trackedPasses[pass];
    auto const& [x, y, z] = tpass.compute_local_size;
    CHECK_MSG(x > 0 && y > 0 && z > 0, "Pass {} does not have a valid compute local size", tpass.name);
    return { x,y,z };
}
void Renderer::CmdDispatch(
    const PassHandle pass, RHICommandList* cmd,
    const RHIExtent3D thread_size
) const
{
    CHECK(m_state == State::Execute);    
    auto const& local_size = CmdGetComputeLocalSize(pass);
    cmd->Dispatch(
        (thread_size.x + local_size.x - 1) / local_size.x,
        (thread_size.y + local_size.y - 1) / local_size.y,
        (thread_size.z + local_size.z - 1) / local_size.z
    );
}
