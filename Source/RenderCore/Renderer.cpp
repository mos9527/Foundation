#include <algorithm>
#include <filesystem>
#include <fstream>

#include <Math/Math.hpp>
#include <tracy/Tracy.hpp>

#include "Renderer.hpp"
#include "Shader.hpp"
using namespace Foundation::Core;
using namespace Foundation::RenderCore;

// Help messages
const char* kShaderDescriptorBindingErrorHelp = "This can be caused by one of the following:\n"
"   - Parameter is optimized-out, and the binding is kept as is.\n"
"   - Multiple entrypoints in the same shader, but they don't access the same parameters.\n"
"Tips:\n"
"   Try separating the entrypoints into different shader files, or sort the binding declarations"
"so that the used bindings are continuous from 0.";

Renderer::Renderer(RendererDesc const& desc, RHIApplicationObjectHandle<RHIDevice> device, RHIDeviceObjectHandle<RHISwapchain> swapchain, Allocator* allocator)
    : m_state(State::Undefined), m_allocator(allocator), m_desc(desc), m_swaps(m_allocator),
      m_device(device), m_swapchain(swapchain), m_executeArena(m_allocator, kExecuteArenaSize), m_executeAlloc(m_executeArena), m_waitIdle(device.Get()) {
    m_graphicsQueue = m_device->GetDeviceQueue(RHIDeviceQueueType::Graphics);
    m_graphicsQueue->DebugSetObjectName("Graphics Queue");
    m_computeQueue = m_device->GetDeviceQueue(RHIDeviceQueueType::Compute);
    m_computeQueue->DebugSetObjectName("Compute Queue");
    m_graphicsCmdPool = m_device->CreateCommandPool(RHICommandPool::PoolDesc{
        .queue = RHIDeviceQueueType::Graphics,
        .type = RHICommandPoolType::Persistent
    });
    m_graphicsCmdPool->DebugSetObjectName("Main Command Pool");
    if (m_desc.async) {
        m_computeCmdPool = m_device->CreateCommandPool(RHICommandPool::PoolDesc{
            .queue = RHIDeviceQueueType::Compute,
            .type = RHICommandPoolType::Persistent
            });
        m_computeCmdPool->DebugSetObjectName("Async Compute Command Pool");
    }
    LOG_RUNTIME(Renderer, info, "** Renderer Init **");
    LOG_RUNTIME(Renderer, info, "Async Compute: {}", m_desc.async);
    LOG_RUNTIME(Renderer, info, "Presentation: {}", m_desc.present);
}

RHITextureSubresourceRange TrackedResource::SubresourceState::ToRange() const
{
    {
        return RHITextureSubresourceRange{
            .layer = {
                .aspect = aspect,
                .mip_level = static_cast<uint32_t>(mip),
                .base_array_layer = static_cast<uint32_t>(layer),
                .layer_count = 1
            },
            .mip_count = 1
        };
    }
}

TrackedResource::TrackedResource(const ResourceHandle handle, StringView name, const ResourceDefinition& resourceDesc, Allocator* alloc)
: handle(handle), name(name), desc(resourceDesc), lastSubresourceStates(alloc) {
    // Init texture tracking states
    auto update_texture_desc = [&](RHITextureDesc const& texture_desc) {
        textureLayers = texture_desc.array_layers;
        textureMips = texture_desc.mip_levels;
        lastSubresourceStates.resize(textureMips * textureLayers * kTextureAspectCount);
        for (uint32_t mip = 0; mip < textureMips; ++mip)
        {
            for (uint32_t layer = 0; layer < textureLayers; ++layer)
            {
                for (uint32_t aspect = 0; aspect < kTextureAspectCount; ++aspect)
                {
                    uint32_t i = mip * (textureLayers * kTextureAspectCount) + layer * kTextureAspectCount + aspect;
                    auto& state = lastSubresourceStates[i];
                    state.aspect = RHITextureAspectFlag(1u << aspect);
                    state.mip = mip, state.layer = layer;
                }
            }
        }
    };
    desc.visit(
        [&](RHITextureDesc const& tex) { update_texture_desc(tex); },
        [&](RHIDeviceObjectHandle<RHITexture> const& tex) { update_texture_desc(tex->m_desc); },
        [&](const RHITexture* const tex) { update_texture_desc(tex->m_desc); }
    );
}

TrackedPass::TrackedPass(Allocator* alloc, const PassHandle handle, StringView name, RHIDeviceQueueType queue, UniquePtr<RenderPass> renderPass)
        : name(name), handle(handle), queue(queue),
        textureUsages(alloc), bufferUsages(alloc), resources(alloc), texviews(alloc),
        shaders(alloc),
        tex_bindings(alloc), buf_bindings(alloc),
        samplers(alloc),
        push_constants(alloc), rtvs(alloc),
        vertex_input_bindings(alloc), vertex_input_attributes(alloc),
        pass(std::move(renderPass)),
        desc_layouts(alloc), p_desc_layouts(alloc),
        desc_sets(alloc), p_desc_sets(alloc),
        external_sets(alloc), external_desc_sets(alloc)
{
};
void Renderer::BeginSetup() {
    CHECK_MSG(m_state == State::Undefined || m_state == State::PostSetup, "Bad Setup state. Current state is {}", m_state);
    m_state = State::Setup;
    m_setup = ConstructUnique<SetupContext>(m_allocator, m_allocator);
    if (m_desc.present)
        SetSwapchain(m_swapchain);
    else
        SetFrameSyncObjects();
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
    if (resource.lastBufferState.producer != kInvalidHandle)
        m_setup->add_edge(pass, resource.lastBufferState.producer, handle);
    // Set producer
    if (access & kAllShaderWrites || (pass != kInvalidHandle && m_setup->trackedPasses[pass].always_produces))
        resource.lastBufferState.producer = pass;
    m_setup->trackedPasses[pass].bufferUsages.emplace_back(handle, access, stage);
    m_setup->trackedPasses[pass].resources.emplace_back(handle);
    m_setup->trackedPasses[pass].pass_stages |= stage;
}
void Renderer::DeclareTextureAccess(
    PassHandle pass, ResourceHandle handle, RHIPipelineStage stage, RHITextureSubresourceRange range, RHIResourceAccess access, RHITextureLayout layout) const
{
    CHECK(m_state == State::Setup);
    auto& resource = m_setup->trackedResources[handle];
    // Check for overlap
    CHECK_MSG(range.layer.aspect.value, "Access aspect must be defined on resource {} when declared.", resource.name);
    auto [mip_begin, mip_end] = range.GetMipLevelRange();
    auto [layer_begin, layer_end] = range.GetArrayLayerRange();
    for (auto const& [h, _access, _stage, r, _layout] : m_setup->trackedPasses[pass].textureUsages) {
        if (h == handle) {
            if (r.layer.aspect == range.layer.aspect)
            {
                auto [r_mip_begin, r_mip_end] = r.GetMipLevelRange();
                auto [r_layer_begin, r_layer_end] = r.GetArrayLayerRange();
                // Mip intersects
                if (!(mip_end < r_mip_begin || mip_begin > r_mip_end)) {
                    // Layer intersects
                    CHECK_MSG(layer_end < r_layer_begin || layer_begin > r_layer_end,"Overlap detected. Texture access must be disjoint.");
                }
            }
        }
    }
    // Do this for all sub resources in range
    for (auto& sta : resource.GetLastSubresourceStateOf(range)) {
        // Add edge
        if (sta.producer != kInvalidHandle)
            m_setup->add_edge(pass, sta.producer, handle);
        // Set producer
        if (access & kAllShaderWrites || (pass != kInvalidHandle && m_setup->trackedPasses[pass].always_produces))
            sta.producer = pass;
    }
    m_setup->trackedPasses[pass].textureUsages.emplace_back(handle, access, stage, range, layout);
    m_setup->trackedPasses[pass].resources.emplace_back(handle);
    m_setup->trackedPasses[pass].pass_stages |= stage;
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
void Renderer::BindDescriptorSet(PassHandle pass, StringView bind_point, RHIDeviceDescriptorSet* descriptor_set, RHIDeviceDescriptorSetLayout* layout)
{
    CHECK(m_state == State::Setup);
    m_setup->trackedPasses[pass].external_sets.emplace_back(descriptor_set, layout, bind_point);
}
ResourceHandle Renderer::BindTextureSRV(
    PassHandle pass, ResourceHandle texture,
    StringView shader_name, RHIPipelineStage stage,
    RHITextureViewDesc const& desc
) const
{
    CHECK(m_state == State::Setup);
    CHECK_MSG(desc.range.IsValid(), "Binding SRV on {} is of invalid range! Did you specify `desc.range`?", m_setup->trackedResources[texture].name);
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
    CHECK_MSG(desc.range.IsValid(), "Binding UAV on {} is of invalid range! Did you specify `desc.range`?", m_setup->trackedResources[texture].name);
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
    CHECK_MSG(desc.range.IsValid(), "Binding RTV on {} is of invalid range! Did you specify `desc.range`?", m_setup->trackedResources[texture].name);
    auto& tpass = m_setup->trackedPasses[pass];
    CHECK_MSG(tpass.queue == RHIDeviceQueueType::Graphics, "RTV (Render Target Views) are only supported on Graphics queues");
    RHITextureAspectFlag kRTVBits = RHITextureAspectFlagBits::Color;
    CHECK_MSG((desc.range.layer.aspect | kRTVBits == kRTVBits) && (desc.range.layer.aspect & kRTVBits),
        "RTV view must have exactly one layer, and the access flag must be Color.");
    DeclareTextureAccess(pass, texture,
        RHIPipelineStageBits::RenderTargetOutput,
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
    CHECK_MSG(desc.range.IsValid(), "Binding DSV on {} is of invalid range! Did you specify `desc.range`?", m_setup->trackedResources[texture].name);
    auto& tpass = m_setup->trackedPasses[pass];
    CHECK_MSG(tpass.queue == RHIDeviceQueueType::Graphics, "DSV (Depth Stencil Views) are only supported on Graphics queues");
    RHITextureAspectFlag kDSVBits = RHITextureAspectFlagBits::Depth | RHITextureAspectFlagBits::Stencil;
    CHECK_MSG((desc.range.layer.aspect | kDSVBits == kDSVBits) && (desc.range.layer.aspect & kDSVBits),
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
    CHECK_MSG(range.IsValid(), "Binding CopyDst on {} is of invalid range! Did you specify `desc.range`?", m_setup->trackedResources[texture].name);
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
    CHECK_MSG(range.IsValid(), "Binding CopySrc on {} is of invalid range! Did you specify `desc.range`?", m_setup->trackedResources[texture].name);
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
        for (const auto& v : m_setup->graph[u] | Views::keys) {
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
        Ranges::sort(resources);
        resources.erase(Ranges::unique(resources).begin(), resources.end());
        for (auto res : resources) {
            auto& tres = m_setup->trackedResources[res];
            if (pass.queue == RHIDeviceQueueType::Graphics)
                tres.graphics_usage = true;
            if (pass.queue == RHIDeviceQueueType::Compute)
                tres.compute_usage = true;
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
        Ranges::sort(exec.begin() + i, exec.begin() + j, [&](PassHandle a, PassHandle b) {
            return m_setup->trackedPasses[a].handle < m_setup->trackedPasses[b].handle;
        });
    }
    // Group passes by queue
    auto& exec_group = m_setup->executionGroups;
    for (PassHandle i = 0, j = 0; i < exec.size();i = j)
    {
        while (j < exec.size() && m_setup->trackedPasses[exec[j]].queue == m_setup->trackedPasses[exec[i]].queue)
            j++;
        auto& group = exec_group.emplace_back(exec_group.size(), m_setup->trackedPasses[exec[i]].queue, m_allocator);
        group.passes.insert(group.passes.end(), exec.begin() + i, exec.begin() + j);
        // Collect dependencies
        for (auto pass : exec_group.back().passes) {
            auto& tpass = m_setup->trackedPasses[pass];
            tpass.group_index = group.group_index;
            group.resources.insert(group.resources.end(), tpass.resources.begin(), tpass.resources.end());
            group.all_stages |= tpass.pass_stages;
        }
        // Sort and unique
        Ranges::sort(group.resources);
        group.resources.erase(Ranges::unique(group.resources).begin(), group.resources.end());
        if (group.queue == RHIDeviceQueueType::Graphics)
            m_setup->executionAnyGraphics = true;
        else if (group.queue == RHIDeviceQueueType::Compute)
            m_setup->executionAnyCompute = true;
    }
    // Assign last Graphics/Compute group
    {
        auto it = Ranges::find_if(m_setup->executionGroups | Views::reverse, [](auto const& g) {
            return g.queue == RHIDeviceQueueType::Graphics;
        });
        if (it != m_setup->executionGroups.rend())
            it->is_last_graphics = true;
        it = Ranges::find_if(m_setup->executionGroups | Views::reverse, [](auto const& g) {
            return g.queue == RHIDeviceQueueType::Compute;
        });
        if (it != m_setup->executionGroups.rend())
            it->is_last_compute = true;
    }
    LOG_RUNTIME(Renderer, debug, "** Render Graph GraphViz **\n{}", DbgDumpGraphviz());
    LOG_RUNTIME(Renderer, debug, "** Render Graph Execution Order **\n{}", DbgDumpActivePasses());
    LOG_RUNTIME(Renderer, debug, "** Render Graph Execution Groups **\n{}", DbgDumpExecutionGroups());
}
/* -- PSO -- */
void Renderer::BuildPipelineState(PassHandle pass) {
    auto& tracked = m_setup->trackedPasses[pass];
    Vector<RHIPipelineState::PipelineStateDesc::ShaderStage> pso_stages(m_allocator);
    // Load shader bytecode
    if (tracked.shaders.empty())
        return; // Pass with no shaders
    LOG_RUNTIME(Renderer, debug, "** Building PSO for {} [{}] **", tracked.name, pass);
    Vector<char> data(m_allocator);
    Map<std::filesystem::path, RHIDeviceScopedObjectHandle<RHIShaderModule>> shaders(m_allocator);
    Map<std::filesystem::path, UniquePtr<Shader>> reflections(m_allocator);
    for (auto const& [shader_path, entry_point, stage] : tracked.shaders) {
        if (!shaders.contains(shader_path)) {
            LOG_RUNTIME(Renderer, debug, "Loading shader {}", shader_path.string());
            Native::ReadFile(shader_path, data);
            reflections.emplace(
                shader_path,
                ConstructUnique<Shader>(m_allocator, data, m_allocator)
            );
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
                    .shader_module = module.Get()
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
    Map<String, RHIDeviceDescriptorSet*> var_ext_sets(m_allocator);
    // Textures
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
    // Buffers
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
    // Samplers
    for (auto& [sampler_handle, binding] : tracked.samplers) {
        auto it = var_types.find(binding);
        if (it == var_types.end())
            var_types[binding] = RHIDescriptorType::Sampler, var_samplers[binding] = sampler_handle;
        else {
            auto& dtype_prev = it->second;
            auto& sampler_handle = var_samplers[binding];
            CHECK(dtype_prev == RHIDescriptorType::Sampler && sampler_handle == sampler_handle);
        }
    }
    // External sets (e.g. @ref TexturePool)
    for (auto& [desc_set, desc_set_layout, binding] : tracked.external_sets) {
        var_ext_sets[binding] = desc_set;
        // We don't create anything for the set - but do resolve these
        // so we can map them later on
        if (var_bind_points.contains(binding))
            tracked.external_desc_sets.emplace_back(var_bind_points[binding].first, desc_set, desc_set_layout);
    }
    Ranges::sort(tracked.external_desc_sets);
    tracked.external_desc_sets.erase(Ranges::unique(tracked.external_desc_sets).begin(), tracked.external_desc_sets.end());
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
    {
        if (!var_ext_sets.contains(name))
            bindings.emplace_back( bind, name );
    }
    Ranges::sort(bindings);
    // Separate into descriptor sets
    Vector<RHIDeviceDescriptorSetLayoutDesc::Binding> set_bindings(m_allocator);
    for (const auto& binding : bindings | Views::values) {
        // TODO: Descriptor Arrays?
        // Not currently used by Renderer APIs - and for use cases like bindless,
        // we have @ref BindDescriptorSet to bind a pre-made descriptor set.
        CHECK_MSG(var_types.contains(binding) || var_ext_sets.contains(binding), "Binding {} is not bound by pass {}, but is used by one of its shaders.", binding, tracked.name);
        set_bindings.push_back({ .count = 1, .stage = RHIShaderStageBits::All, .type = var_types[binding] });
    }
    // Check if the external set conflicts with our own bindings
    for (auto const& [set, ptr, layout_ptr] : tracked.external_desc_sets)
    {
        auto it = Ranges::find_if(bindings, [set](auto const& b)
        {
            return b.first.first == set;
        });
        if(it != bindings.end())
        {
            auto e_it = Ranges::find_if(tracked.external_sets, [set, ptr](auto const& e)
            {
                return std::get<0>(e) == ptr;
            });
            CHECK_MSG(false,
                "External descriptor set used by shader at set {} (used by '{}') conflicts with bindings declared by pass {}, which is declared internally. Declare different set usage _in shader_ for usage!",
                set, std::get<2>(*e_it), tracked.name
            );
        }
    }
    // Check if our first set is not 0
    if (!bindings.empty() && bindings[0].first.first != 0)
    {
        LOG_RUNTIME(BuildPipelineState, err,
            "Binding set numbers must start from 0. Error at set {} binding {} in pass {}.",
            bindings[0].first.first, bindings[0].first.second, tracked.name
        );
        LOG_RUNTIME(BuildPipelineState, info, kShaderDescriptorBindingErrorHelp);
        CHECK_MSG(false, "Binding set numbers must start from 0.");
    }
    for (uint32_t i = 0, j = 0; i < bindings.size(); i = j) {
        uint32_t set = bindings[i].first.first;
        // Check if our first binding is not 0
        if (bindings[i].first.second != 0)
        {
            LOG_RUNTIME(BuildPipelineState, err,
                "Binding numbers must start from 0 in each descriptor set. Error at set {} binding {} in pass {}.",
                set, bindings[i].first.second, tracked.name
            );
            LOG_RUNTIME(BuildPipelineState, info, kShaderDescriptorBindingErrorHelp);
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
        tracked.p_desc_layouts.emplace_back(tracked.desc_layouts.back().Get());
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
            auto const& [binding_set, binding] = bind;
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
    // Add external sets
    // We've already established that these would not conflict, and has already been sorted
    for (auto const& [set, ptr, layout_ptr] : tracked.external_desc_sets)
    {
        tracked.p_desc_sets.emplace_back(ptr);
        tracked.p_desc_layouts.emplace_back(layout_ptr);
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
        .descriptor_set_layouts = tracked.p_desc_layouts,
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
    // !! TODO: Overlap transient resources to with non-overlapping lifetimes with aliasing
    for (const auto& handle : m_setup->activeResources | Views::keys) {
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
        for (const auto& key : pass.samplers | Views::keys)
            activeSamplers.push_back(key);
    }
    // Instantiate views
    Ranges::sort(activeViews);
    activeViews.erase(Ranges::unique(activeViews).begin(), activeViews.end());
    m_resources->fit(activeViews.size());
    for (auto hdl : activeViews) {
        auto [rhdl, desc] = m_setup->trackedViews[hdl];
        auto res = DerefResource(rhdl).Get<RHITexture*>();
        m_resources->views[hdl] = res->CreateTextureView(desc);
    }
    // Instantiate samplers
    Ranges::sort(activeSamplers);
    activeSamplers.erase(Ranges::unique(activeSamplers).begin(), activeSamplers.end());
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
void Renderer::SetFrameSyncObjects() {
    while (m_swaps.size() < m_frameSwaps)
        m_swaps.emplace_back(m_swaps.size(), m_allocator);
    for (size_t i = 0; i < m_frameSwaps; i++) {
        m_swaps[i].render = m_device->CreateSemaphore(false);
        m_swaps[i].render->DebugSetObjectName(fmt::format("Render Semaphore of Swap {}", i).c_str());
        m_swaps[i].present = m_device->CreateSemaphore(false);
        m_swaps[i].present->DebugSetObjectName(fmt::format("Present Semaphore of Swap {}", i).c_str());
        m_swaps[i].graphics_fence = m_device->CreateFence(true);
        m_swaps[i].graphics_fence->DebugSetObjectName(fmt::format("Graphics Fence of Swap {}", i).c_str());
        m_swaps[i].compute_fence = m_device->CreateFence(true);
        m_swaps[i].compute_fence->DebugSetObjectName(fmt::format("Compute Fence of Swap {}", i).c_str());
    }
    m_asyncSemaphore = m_device->CreateSemaphore(true);
    m_asyncSemaphore->DebugSetObjectName(fmt::format("Async Compute Semaphore").c_str());
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
            .format = swapchain->m_desc.format,
            .range = RHITextureSubresourceRange::Create()
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
void Renderer::ExecuteCheckResourceStates(Span<PassHandle> passes, RHIDeviceQueueType currentQueue,
                                          Vector<Pair<size_t, bool>>* outGroups)
{
    ZoneScoped;
    if (outGroups)
        outGroups->clear();
    auto PushProducerGroup = [&](PassHandle handle, size_t lastFrame)
    {
        if (handle == kInvalidHandle)
            return;
        auto& pass = m_setup->trackedPasses[handle];
        if (outGroups)
            outGroups->emplace_back(pass.group_index, lastFrame != m_frame);
    };
    auto CheckTransition = [&](StringView name, RHIPipelineStage stage)
    {
        if (currentQueue == RHIDeviceQueueType::Compute)
            CHECK_MSG(!(stage & kComputeStagesMask), "FIXME-Transition: Barrier incompatible on resource {}", name);
    };
    for (auto pass_handle : passes)
    {
        auto const& pass = m_setup->trackedPasses[pass_handle];
        for (auto [hdl, access, stage, range, layout] : pass.textureUsages)
        {
            auto& tres = m_setup->trackedResources[hdl];
            for (auto const& sta : tres.GetLastSubresourceStateOf(range))
            {
                CheckTransition(tres.name, sta.stage);
                PushProducerGroup(sta.lastExecutor, sta.lastExecuteFrame);
            }
        }
        if (pass.write_backbuffer)
            CheckTransition("Backbuffer", RHIPipelineStageBits::RenderTargetOutput);
        for (auto [hdl, access, stage] : pass.bufferUsages)
        {
            auto& tres = m_setup->trackedResources[hdl];
            CheckTransition(tres.name, tres.lastBufferState.stage);
            PushProducerGroup(tres.lastBufferState.lastExecutor, tres.lastBufferState.lastExecuteFrame);
        }
    }
    // Sort and unique cross-queue groups
    if (outGroups && !outGroups->empty())
    {
        Ranges::sort(*outGroups);
        outGroups->erase(Ranges::unique(*outGroups).begin(), outGroups->end());
    }
}
void Renderer::BeginExecute()
{
    CHECK_MSG(m_state == State::PostSetup, "Renderer bad state ({}). Did you call EndSetup() or EndExecute()?", m_state);
    ZoneScoped;
    m_executeAlloc.Reset(m_executeArena), m_state = State::Execute;
    Vector<RHIDeviceObjectHandle<RHIDeviceFence>> wait_fences(m_executeAlloc.Ptr());
    if (m_setup->executionAnyGraphics)
        wait_fences.push_back(m_swaps[m_currentSync].graphics_fence);
    if (m_setup->executionAnyCompute)
        wait_fences.push_back(m_swaps[m_currentSync].compute_fence);
    {
        ZoneScopedN("Wait for GPU");
        m_device->WaitForFences(wait_fences, true, -1);
        m_device->ResetFences(wait_fences);
    }
    if (m_desc.present)
    {
        m_currentSwap = m_swapchain->GetNextImage(
            -1, m_swaps[m_currentSync].present, {}
        );
    }
}
void Renderer::ExecuteBarrierSubresourceState(PassHandle pass, RHITexture* res,  TrackedResource::SubresourceState& sta,
                                              RHIResourceAccess access,
                                              RHIPipelineStage stage, RHITextureLayout layout, RHICommandList* cmd)
{
    ZoneScoped;
    if (sta.access == access && sta.stage == stage && sta.layout == layout)
        return;
    cmd->SetImageTransition(
        res,
        {
            .src_access = sta.access,
            .dst_access = access,
            .src_stage = sta.stage,
            .dst_stage = stage,
            .src_img_layout = sta.layout,
            .dst_img_layout = layout,
            .src_img_range = sta.ToRange(),
        }
    );
    sta.access = access;
    sta.stage = stage;
    sta.layout = layout;
    sta.lastExecutor = pass;
    sta.lastExecuteFrame = m_frame;
}
void Renderer::ExecuteBarrierSubresource(PassHandle pass, TrackedResource& tres,
                                         RHITextureSubresourceRange const& range, RHIResourceAccess access,
                                         RHIPipelineStage stage, RHITextureLayout layout, RHICommandList* cmd)
{
    ZoneScoped;
    CHECK_MSG(m_state == State::Execute, "Renderer bad state ({}). Did you call BeginExecute()?", m_state);
    RHITexture* res = DerefResource(tres.handle).Get<RHITexture*>();
    bool any_range = false;
    cmd->DebugBegin(tres.name.c_str());
    for (auto& sta : tres.GetLastSubresourceStateOf(range)) {
        any_range = true;
        ExecuteBarrierSubresourceState(pass, res, sta, access, stage, layout, cmd);
    }
    cmd->DebugEnd();
    CHECK_MSG(any_range, "FIXME-ExecuteBarrierSubresource: Failed to match resource range on {}",tres.name);
}
void Renderer::ExecuteBarrierBuffer(PassHandle pass, TrackedResource& tres, RHIResourceAccess access,
                                    RHIPipelineStage stage, RHICommandList* cmd)
{
    ZoneScoped;
    CHECK_MSG(m_state == State::Execute, "Renderer bad state ({}). Did you call BeginExecute()?", m_state);
    RHIBuffer* res = DerefResource(tres.handle).Get<RHIBuffer*>();
    if (tres.lastBufferState.access == access && tres.lastBufferState.stage == stage)
        return;
    cmd->DebugBegin(tres.name.c_str());
    cmd->SetBufferTransition(
        res,
        {
            .src_access = tres.lastBufferState.access,
            .dst_access = access,
            .src_stage = tres.lastBufferState.stage,
            .dst_stage = stage,
        }
        );
    cmd->DebugEnd();
    tres.lastBufferState.access = access;
    tres.lastBufferState.stage = stage;
    tres.lastBufferState.lastExecutor = pass;
    tres.lastBufferState.lastExecuteFrame = m_frame;
}
void Renderer::ExecuteBarriers(TrackedPass& pass, RHICommandList* cmd)
{
    ZoneScoped;
    CHECK_MSG(m_state == State::Execute, "Renderer bad state ({}). Did you call BeginExecute()?", m_state);
    // At this point the pass execution order has been determined
    // (execution) and so are the resources' access patterns.
    // Minimal synchronization barriers would always be the most
    // optimal.
    // Textures
    // These are always disjoint ranges
    for (auto [hdl, access, stage, range, layout] : pass.textureUsages)
    {
        auto& tres = m_setup->trackedResources[hdl];
        ExecuteBarrierSubresource(pass.handle, tres, range, access, stage, layout, cmd);
    }
    // Backbuffer
    // A special case with known usages.
    // We never create resource per-swap so tracking Back buffers by passes
    // is not possible. The BB is also opaque to the passes for the same reasons.
    // Synchronization for other resources are already handled above,
    // and should eliminate any redundant per-pass resource creation.
    if (pass.write_backbuffer)
    {
        CHECK_MSG(pass.queue == RHIDeviceQueueType::Graphics, "Backbuffer can only be used in Graphics queue");
        const RHIResourceAccess rt_access = RHIResourceAccessBits::RenderTargetWrite;
        const RHITextureLayout rt_layout = RHITextureLayout::RenderTarget;
        const RHIPipelineStage rt_stage = RHIPipelineStageBits::RenderTargetOutput;
        auto& tres = m_setup->trackedResources[m_swaps[m_currentSwap].rt_handle];
        ExecuteBarrierSubresource(pass.handle, tres, RHITextureSubresourceRange::Create(), rt_access, rt_stage,
                                  rt_layout, cmd);
    }
    // Buffers
    // These are always global i.e. at most one per buffer per pass.
    for (auto [hdl, access, stage] : pass.bufferUsages)
    {
        auto& tres = m_setup->trackedResources[hdl];
        ExecuteBarrierBuffer(pass.handle, tres, access, stage, cmd);
    }
}
void Renderer::ExecuteAcquireQueueResources(RHIDeviceQueueType currentQueue, size_t groupIndex, RHICommandList* cmd)
{
    ZoneScoped;
    auto& groups = m_setup->executionGroups;
    if (groups.size() <= 1) // e.g. No Async Compute
        return;
    cmd->DebugBegin("<Group Resource Acquire>");
    cmd->BeginTransition();
    uint32_t currentQueueIndex = ExecuteGetQueueIndex(currentQueue);
    for (PassHandle pass : groups[groupIndex].passes)
    {
        auto& tracked = m_setup->trackedPasses[pass];
        for (auto [hdl, access, stage, range, layout] : tracked.textureUsages)
        {
            auto& tres = m_setup->trackedResources[hdl];
            if (!(tres.graphics_usage && tres.compute_usage))
                continue; // Only care about cross-queue resources
            cmd->DebugBegin(tres.name.c_str());
            for (auto& sta : tres.GetLastSubresourceStateOf(range))
            {
                if (sta.lastOwnerQueue == currentQueue)
                    continue;
                if (sta.lastOwnerQueue != RHIDeviceQueueType::Undefined)
                    cmd->SetImageTransition(
                        DerefResource(tres.handle).Get<RHITexture*>(),
                    {
                            .src_img_range = sta.ToRange(),
                            .src_queue_index = ExecuteGetQueueIndex(sta.lastOwnerQueue),
                            .dst_queue_index = currentQueueIndex
                        }
                    );
                sta.lastOwnerQueue = currentQueue;
            }
            cmd->DebugEnd();
        }
        for (auto [hdl, access, stage] : tracked.bufferUsages)
        {
            auto& tres = m_setup->trackedResources[hdl];
            if (!(tres.graphics_usage && tres.compute_usage))
                continue; // Only care about cross-queue resources
            if (tres.lastBufferState.lastOwnerQueue == currentQueue)
                continue;
            cmd->DebugBegin(tres.name.c_str());
            if (tres.lastBufferState.lastOwnerQueue != RHIDeviceQueueType::Undefined)
                cmd->SetBufferTransition(
                    DerefResource(tres.handle).Get<RHIBuffer*>(),
                {
                    .src_queue_index = ExecuteGetQueueIndex(tres.lastBufferState.lastOwnerQueue),
                    .dst_queue_index = currentQueueIndex
                }
                );
            cmd->DebugEnd();
            tres.lastBufferState.lastOwnerQueue = currentQueue;
        }
    }
    cmd->EndTransition();
    cmd->DebugEnd();
}
void Renderer::ExecuteReleaseQueueResources(RHIDeviceQueueType currentQueue, size_t groupIndex, RHICommandList* cmd)
{
    ZoneScoped;
    auto& groups = m_setup->executionGroups;
    if (groups.size() <= 1) // e.g. No Async Compute
        return;
    cmd->DebugBegin("<Group Resource Release>");
    /* -- Pre-transition -- */
    // If the _current_ queue is strictly more capable (i.e. Graphics), transition the resources for
    // the _next_ group which is *now* guaranteed to be less capable (i.e. Compute).
    // Only Compute resources _need_ to be transitioned here beforehand. So we only deal with that
    size_t nextGroupIndex = (groupIndex + 1) % groups.size(); // Next group. Could be the first group if this is the last group
    if (groups[groupIndex].queue == RHIDeviceQueueType::Graphics && groups[nextGroupIndex].queue != currentQueue)
    {
        // Declare that the last pass from this group handled the transition
        PassHandle executorPass = m_setup->executionGroups[groupIndex].passes.back();
        cmd->DebugBegin("<Group Graphics to Compute>");
        cmd->BeginTransition();
        for (PassHandle pass : groups[nextGroupIndex].passes)
        {
            auto& tracked = m_setup->trackedPasses[pass];
            for (auto [hdl, access, stage, range, layout] : tracked.textureUsages)
            {
                auto& tres = m_setup->trackedResources[hdl];
                RHITexture* res = DerefResource(tres.handle).Get<RHITexture*>();
                cmd->DebugBegin(tres.name.c_str());
                for (auto& sta : tres.GetLastSubresourceStateOf(range))
                {
                    // Only deal with resources currently owned by us _once_
                    // This implies that the states (stages) _could_ be one
                    // of ours (e.g. Fragment) and cannot be transitioned by the subsequent (Compute)
                    // group.
                    // Releasing this state SHOULD imply that all queues - no matter capability
                    // can transition it.
                    if (sta.executeTempTransitionFlag)
                        continue; // Only transition once - so the *first* usages of the next group are valid
                    // Subsequent intra-group transitions should always be valid by themselves
                    ExecuteBarrierSubresourceState(executorPass, res, sta, access, stage, layout, cmd);
                    sta.executeTempTransitionFlag = true;
                }
                cmd->DebugEnd();
            }
            for (auto [hdl, access, stage] : tracked.bufferUsages)
            {
                auto& tres = m_setup->trackedResources[hdl];
                // Same as above
                if (tres.lastBufferState.executeTempTransitionFlag)
                    continue;
                cmd->DebugBegin(tres.name.c_str());
                ExecuteBarrierBuffer(executorPass, tres, access, stage, cmd);
                cmd->DebugEnd();
                tres.lastBufferState.executeTempTransitionFlag = true;
            }
            // Reset the flags
            for (auto [hdl, access, stage, range, layout] : tracked.textureUsages)
            {
                auto& tres = m_setup->trackedResources[hdl];
                for (auto& sta : tres.GetLastSubresourceStateOf(range))
                    sta.executeTempTransitionFlag = false;
            }
            for (auto [hdl, access, stage] : tracked.bufferUsages)
            {
                auto& tres = m_setup->trackedResources[hdl];
                tres.lastBufferState.executeTempTransitionFlag = false;
            }
        }
        cmd->EndTransition();
        cmd->DebugEnd();
    } else
    { /* Compute - nop */ }
    // Actually release our resources for subsequent groups
    // Do this for resources to the next queue that may require alternate queue access.
    uint32_t currentQueueIndex = ExecuteGetQueueIndex(currentQueue);
    // Always alternate
    // Not always optimal - but easier than tracking multiple queues across multiple groups across multiple frames
    // The resources that are touched on different queues are usually sparse anyway. And only these are released.
    // Release/Acquire for other resources would be no-ops
    uint32_t nextQueueIndex = ExecuteGetQueueIndex(
        currentQueue == RHIDeviceQueueType::Graphics ?
        RHIDeviceQueueType::Compute :
        RHIDeviceQueueType::Graphics
    );
    cmd->BeginTransition();
    for (PassHandle pass : groups[groupIndex].passes)
    {
        auto& tracked = m_setup->trackedPasses[pass];
        for (auto [hdl, access, stage, range, layout] : tracked.textureUsages)
        {
            auto& tres = m_setup->trackedResources[hdl];
            if (!(tres.graphics_usage && tres.compute_usage))
                continue; // Only care about cross-queue resources
            cmd->DebugBegin(tres.name.c_str());
            for (auto& sta : tres.GetLastSubresourceStateOf(range))
            {
                cmd->SetImageTransition(
                    DerefResource(tres.handle).Get<RHITexture*>(),
                {
                    .src_img_range = sta.ToRange(),
                    .src_queue_index = currentQueueIndex,
                    .dst_queue_index = nextQueueIndex
                }
                );
            }
            cmd->DebugEnd();
        }
        for (auto [hdl, access, stage] : tracked.bufferUsages)
        {
            auto& tres = m_setup->trackedResources[hdl];
            if (!(tres.graphics_usage && tres.compute_usage))
                continue; // Only care about cross-queue resources
            cmd->DebugBegin(tres.name.c_str());
            cmd->SetBufferTransition(
                DerefResource(tres.handle).Get<RHIBuffer*>(),
            {
                .src_queue_index = currentQueueIndex,
                .dst_queue_index = nextQueueIndex
            }
            );
            cmd->DebugEnd();
        }
    }
    cmd->EndTransition();
    cmd->DebugEnd();
}
void Renderer::ExecuteFrame()
{
    ZoneScoped;
    CHECK_MSG(m_state == State::Execute, "Renderer bad state ({}). Did you call BeginExecute()?", m_state);
    auto& passes = m_setup->trackedPasses;
    // Async compute supporting command lists
    int cmd_graphics = 0, cmd_compute = 0;
    RHIDeviceSemaphore* async_semaphore = m_asyncSemaphore.Get();
    RHIDeviceQueue* queue = nullptr;
    RHICommandList* cmd = nullptr;
    auto SetNextGraphicsQueue = [&] {
        ZoneScopedN("Acquire Graphics Cmd");
        CHECK_MSG(cmd_graphics < kMaxCommandListsPerSwap, "FIXME-Async Compute: All transient Graphics Command lists exhausted");
        queue = m_graphicsQueue, cmd = m_swaps[m_currentSync].graphics_cmd_at(cmd_graphics++, m_graphicsCmdPool.Get());
        cmd->Reset(), cmd->Begin();
    };
    auto SetNextComputeQueue = [&] {
        ZoneScopedN("Acquire Compute Cmd");
        CHECK_MSG(cmd_compute < kMaxCommandListsPerSwap, "FIXME-Async Compute: All transient Compute Command lists exhausted");
        queue = m_computeQueue, cmd = m_swaps[m_currentSync].compute_cmd_at(cmd_compute++, m_computeCmdPool.Get());
        cmd->Reset(), cmd->Begin();
    };
    // Execute by groups
    for (auto& group : m_setup->executionGroups)
    {
        const bool is_last = group.group_index == m_setup->executionGroups.size() - 1;
        /* -- Pass Recording -- */
        if (group.queue == RHIDeviceQueueType::Graphics)
            SetNextGraphicsQueue();
        else if (group.queue == RHIDeviceQueueType::Compute)
            SetNextComputeQueue();
        else [[unlikely]]
            throw std::runtime_error("Unhandled queue type");
        // Acquire resources for this queue - which should already been released
        ExecuteAcquireQueueResources(group.queue, group.group_index, cmd);
        // We've previously established that all passes in a group share the same queue
        for (auto pass_handle : group.passes)
        {
            auto& pass = passes[pass_handle];
            ZoneScopedN("Pass Execution");
            ZoneNameF("[%s]", pass.name.c_str());
            if (pass.pass->IsSkipped(pass.handle, this))
                continue;
            /* -- Barriers -- */
            // For textures - we're dealing with Subresource ranges,
            // and for buffer it's always the whole thing.
            // Execution on the states are always single-threaded due
            // to the granularity of these states - the introduction
            // of sync primitives here would incur quite a lot of overhead
            cmd->DebugBegin(pass.name.c_str());
            cmd->DebugBegin("<Resource Barriers>");
            cmd->BeginTransition();
            // Transfer *here* are on the same queue, so ignored
            ExecuteBarriers(pass, cmd);
            {
                ZoneScopedN("Finalize Transitions");
                cmd->EndTransition();
            }
            cmd->DebugEnd();
            // TODO: Only dealing with Record() here can easily introduce parallelism
            //       But - we don't currently have a good CPU async tasking system
            //       yet - which would be the hard part.
            {
                ZoneScopedN("Record");
                pass.pass->Record(pass.handle, this, cmd);
            }
            cmd->DebugEnd();
        }
        // Release resources to the next group (*always* in different queues) if needed
        // since we can *only* do that on this queue
        ExecuteReleaseQueueResources(group.queue, group.group_index, cmd);
        /* -- Submission -- */
        {
            ZoneScopedN("Group Submit");
            // We'd only signal the current group per submit
            auto Counter = [&](size_t ord) { return m_frame * m_setup->executionGroups.size() + ord + 1; };
            RHIDeviceQueue::TimelinePair timeline_signal(async_semaphore, Counter(group.group_index));
            RHIDeviceQueue::TimelinePair timeline_wait(async_semaphore, Counter(group.group_index - 1));
            RHIDeviceFence* fence_ptr = nullptr;
            if (group.is_last_compute)
                fence_ptr = m_swaps[m_currentSync].compute_fence.Get();
            else if (group.is_last_graphics)
                fence_ptr = m_swaps[m_currentSync].graphics_fence.Get();
            if (is_last)
            {
                ZoneScopedN("Final Submit");
                if (!m_desc.present){
                    ZoneScopedN("Submit (No Present)");
                    cmd->End();
                    queue->Submit({
                        .timeline_waits = {{{ timeline_wait }}},
                        .timeline_signals = {{{ timeline_signal }}},
                        .waits_stages = {{{ group.all_stages }}},
                        .cmd_lists = { cmd },
                        .fence = fence_ptr
                    });
                } else {
                    // Last group to submit, and we need to present
                    if (group.queue == RHIDeviceQueueType::Compute)
                    {
                        cmd->End();
                        // Submit compute first
                        queue->Submit({
                            .timeline_waits = {{{ timeline_wait }}},
                            .timeline_signals = {{{ timeline_signal }}},
                            .waits_stages = {{{ group.all_stages }}},
                            .cmd_lists = { cmd },
                            .fence = fence_ptr // Compute
                        });
                        SetNextGraphicsQueue();
                    }
                    // Transition the Backbuffer
                    cmd->DebugBegin("Present");
                    cmd->BeginTransition();
                    ExecuteBarrierSubresource(
                        kInvalidHandle,
                        m_setup->trackedResources[m_swaps[m_currentSwap].rt_handle],
                        RHITextureSubresourceRange::Create(),
                        {},
                        RHIPipelineStageBits::RenderTargetOutput,
                        RHITextureLayout::Present, cmd
                    );
                    cmd->EndTransition();
                    cmd->DebugEnd();
                    cmd->End();
                    {
                        ZoneScopedN("Submit & Present");
                        RHIPipelineStage stages{group.all_stages | RHIPipelineStageBits::RenderTargetOutput};
                        queue->Submit({
                            .timeline_waits = {{{ timeline_wait }}},
                            .timeline_signals = {{{ timeline_signal }}},
                            .waits = {{ m_swaps[m_currentSync].present.Get() }},
                            .waits_stages = {{ stages, stages }},
                            .signals = {{ m_swaps[m_currentSwap].render.Get() }},
                            .cmd_lists = {{ cmd }},
                            .fence = m_swaps[m_currentSync].graphics_fence.Get()
                        });
                        queue->Present({
                            .image_index = m_currentSwap,
                            .swapchain = m_swapchain.Get(),
                            .waits = {{ m_swaps[m_currentSwap].render.Get() }}
                        });
                    }
                }
            } else
            {
                ZoneScopedN("Submit");
                cmd->End();
                queue->Submit({
                    .timeline_waits = {{{ timeline_wait }}},
                    .timeline_signals = {{{ timeline_signal }}},
                    .waits_stages = {{{ group.all_stages }}},
                    .cmd_lists = { cmd },
                    .fence = fence_ptr
                });
            }
        }
    }
}
void Renderer::EndExecute()
{
    ZoneScoped;
    CHECK_MSG(m_state == State::Execute, "Renderer bad state ({}). EndExecute() may only be called once per frame.", m_state);
    m_currentSync = (m_currentSync + 1) % m_frameSwaps;
    m_frame++;
    m_state = State::PostSetup;
    FrameMark;
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
            tpass.p_desc_sets,
            0
        );
    for (auto const& [index, ptr, layout_ptr] : tpass.external_desc_sets)
        CmdBindDescriptorSet(pass, cmd, index, ptr);
}
void Renderer::CmdBindDescriptorSet(PassHandle pass, RHICommandList* cmd, uint32_t index,
                                    RHIDeviceDescriptorSet* descriptor_set) const
{
    CHECK(m_state == State::Execute);
    auto& tpass = m_setup->trackedPasses[pass];
    CHECK_MSG(tpass.pso.IsValid(), "Current pass has no Pipeline state.");
    cmd->BindDescriptorSet(
        tpass.compute_pass ? RHIDevicePipelineType::Compute : RHIDevicePipelineType::Graphics,
        tpass.pso.Get(),
        {{ descriptor_set }},
        index
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
/* -- Debug -- */
String Renderer::DbgDumpGraphviz() const {
    String out;
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
            pass.queue == RHIDeviceQueueType::Graphics ? "#d0e0f0" : "#f0d0e0");
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
    fmt::format_to(std::back_inserter(out), "}}\n");
    out.pop_back();
    return out;
}

String Renderer::DbgDumpActivePasses() const {
    String out;
    for (const auto& idx : m_setup->execution) {
        auto& pass = m_setup->trackedPasses[idx];
        fmt::format_to(
            std::back_inserter(out), "{}: {}, depth={}, ord={}, queue={}, group={}, write_backbuffer={}\n",
            pass.handle,
            pass.name,
            pass.depth,
            pass.ord,
            pass.queue,
            pass.group_index,
            pass.write_backbuffer
        );
    }
    out.pop_back();
    return out;
}

String Renderer::DbgDumpExecutionGroups() const
{
    String out;
    for (const auto& group : m_setup->executionGroups)
    {
        fmt::format_to(
            std::back_inserter(out), "{}: queue={}, stages={:b}, passes=[",
            group.group_index,
            group.queue,
            static_cast<uint32_t>(group.all_stages)
        );
        for (const auto& pass : group.passes)
            fmt::format_to(std::back_inserter(out), "{} ", pass);
        out.pop_back();
        fmt::format_to(std::back_inserter(out), "]\n");
    }
    out.pop_back();
    return out;
}
