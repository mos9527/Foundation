#include <tracy/Tracy.hpp>

#include "Renderer.hpp"
#include "Shader.hpp"
using namespace Foundation::Core;
using namespace Foundation::RenderCore;

// Help messages
const char* kShaderDescriptorBindingErrorHelp =
    "This can be caused by one of the following:\n"
    "   - Parameter is optimized-out, and the binding is kept as is.\n"
    "   - Multiple entrypoints in the same shader, but they don't access the same parameters.\n"
    "Tips:\n"
    "   Try separating the entrypoints into different shader files, or sort the binding declarations"
    "so that the used bindings are continuous from 0.";

Renderer::Renderer(RendererDesc const& desc, RHIApplicationObjectHandle<RHIDevice> device,
                   RHIDeviceObjectHandle<RHISwapchain> swapchain, Allocator* allocator) :
    mState(State::Undefined), mAllocator(allocator), mDesc(desc), mSwaps(mAllocator), mDevice(device),
    mSwapchain(swapchain), mExecuteArena(mAllocator, kExecuteArenaSize), mExecuteAlloc(mExecuteArena),
    mExecuteThreadPool(mDesc.renderThreads, kMaxCommandListsPerThread * 2, allocator, "Renderer"),
    mExecutePerSwapCmds(allocator), mWaitIdle(device.Get())
{
    mGraphicsQueue = mDevice->GetDeviceQueue(RHIDeviceQueueType::Graphics);
    mGraphicsQueue->DebugSetObjectName("Graphics Queue");
    mComputeQueue = mDevice->GetDeviceQueue(RHIDeviceQueueType::Compute);
    mComputeQueue->DebugSetObjectName("Compute Queue");
    LOG_RUNTIME(Renderer, info, "** Renderer Init **");
    LOG_RUNTIME(Renderer, info, "Async Compute: {}", mDesc.async);
    LOG_RUNTIME(Renderer, info, "Presentation: {}", mDesc.present);
}

void Renderer::BeginSetup()
{
    CHECK_MSG(mState == State::Undefined || mState == State::PostSetup, "Bad Setup state. Current state is {}", mState);
    mState = State::Setup;
    mSetup = ConstructUnique<RendererSetup>(mAllocator, mAllocator);
    if (mDesc.present)
        SetSwapchain(mSwapchain);
    else
        SetFrameSyncObjects();
}
ResourceHandle Renderer::CreateTextureView(PassHandle pass, ResourceHandle handle, RHITextureViewDesc const& desc) const
{
    CHECK(mState == State::Setup);
    mSetup->trackedViews.emplace_back(handle, desc);
    ResourceHandle hdl = mSetup->trackedViews.size() - 1;
    mSetup->trackedPasses[pass].texviews.emplace_back(hdl);
    return hdl;
}
ResourceHandle Renderer::CreateSampler(RHIDeviceSampler::SamplerDesc const& desc) const
{
    CHECK(mState == State::Setup);
    mSetup->trackedSamplers.emplace_back(desc);
    return mSetup->trackedSamplers.size() - 1;
}
void Renderer::DeclareBufferAccess(PassHandle pass, ResourceHandle handle, RHIPipelineStage stage,
                                   RHIResourceAccess access) const
{
    CHECK(mState == State::Setup);
    auto& resource = mSetup->trackedResources[handle];
    // Check for overlap
    for (auto const& [h, _access, _stage] : mSetup->trackedPasses[pass].bufferUsages)
        if (h == handle)
            throw std::runtime_error("Overlap detected. Buffer access must be global.");
    // Add edge
    if (resource.lastBufferState.producer != kInvalidHandle)
        mSetup->add_edge(pass, resource.lastBufferState.producer, handle);
    // Set producer
    if (access & kAllShaderWrites)
        resource.lastBufferState.producer = pass;
    mSetup->trackedPasses[pass].bufferUsages.emplace_back(handle, access, stage);
    mSetup->trackedPasses[pass].resources.emplace_back(handle);
    mSetup->trackedPasses[pass].piplineStages |= stage;
}
void Renderer::DeclareTextureAccess(PassHandle pass, ResourceHandle handle, RHIPipelineStage stage,
                                    RHITextureSubresourceRange range, RHIResourceAccess access,
                                    RHITextureLayout layout) const
{
    CHECK(mState == State::Setup);
    auto& resource = mSetup->trackedResources[handle];
    // Do this for all sub resources in range
    for (auto& sta : resource.GetLastSubresourceStateOf(range))
    {
        // Add edge
        if (sta.producer != kInvalidHandle)
            mSetup->add_edge(pass, sta.producer, handle);
        // Set producer
        if (access & kAllShaderWrites)
            sta.producer = pass;
    }
    mSetup->trackedPasses[pass].textureUsages.emplace_back(handle, access, stage, range, layout);
    mSetup->trackedPasses[pass].resources.emplace_back(handle);
    mSetup->trackedPasses[pass].piplineStages |= stage;
}

/* -- binding -- */
void Renderer::BindShader(PassHandle pass, RHIShaderStage stage, StringView entry_point,
                          Native::Path const& shader_path) const
{
    CHECK(mState == State::Setup);
    CHECK_MSG(stage.is_bitmask(), "Only one stage can be bound to a shader per pass");
    for (auto const& [path, ep, st] : mSetup->trackedPasses[pass].shaders)
        CHECK_MSG(!(st & stage),
                  "Shader stage {} already bound to {} in this pass. There can be at most one shader program per "
                  "shader stage per pass",
                  st, path);
    mSetup->trackedPasses[pass].shaders.emplace_back(shader_path, entry_point, stage);
}
void Renderer::BindVertexInput(PassHandle pass, RHIPipelineState::PipelineStateDesc::VertexInput const& info) const
{
    CHECK(mState == State::Setup);
    mSetup->trackedPasses[pass].vertexInputBindings.insert(mSetup->trackedPasses[pass].vertexInputBindings.end(),
                                                           info.bindings.begin(), info.bindings.end());
    mSetup->trackedPasses[pass].vertexInputAttributes.insert(mSetup->trackedPasses[pass].vertexInputAttributes.end(),
                                                             info.attributes.begin(), info.attributes.end());
}
void Renderer::BindPushConstant(PassHandle pass, RHIShaderStage stage, size_t offset, size_t size) const
{
    CHECK(mState == State::Setup);
    for (auto const& [st, _offset, _size] : mSetup->trackedPasses[pass].pushConstants)
        CHECK_MSG(!(st & stage),
                  "Shader stage {} already has Push Constant bound in this pass. There can be only one Push Constant "
                  "configuration per shader stage per pass.",
                  st);
    mSetup->trackedPasses[pass].pushConstants.emplace_back(stage, offset, size);
}
void Renderer::BindBufferUniform(PassHandle pass, ResourceHandle buffer, RHIPipelineStage stage,
                                 StringView bind_point) const
{
    CHECK(mState == State::Setup);
    DeclareBufferAccess(pass, buffer, stage, RHIResourceAccessBits::UniformRead);
    mSetup->trackedPasses[pass].bufferBindings.emplace_back(buffer, RHIDescriptorType::UniformBuffer, bind_point);
    mSetup->bindingCounts[RHIDescriptorType::UniformBuffer]++;
}
void Renderer::BindBufferStorageRead(PassHandle pass, ResourceHandle buffer, RHIPipelineStage stage,
                                     StringView bind_point) const
{
    CHECK(mState == State::Setup);
    DeclareBufferAccess(pass, buffer, stage, RHIResourceAccessBits::ShaderRead);
    mSetup->trackedPasses[pass].bufferBindings.emplace_back(buffer, RHIDescriptorType::StorageBuffer, bind_point);
    mSetup->bindingCounts[RHIDescriptorType::StorageBuffer]++;
}
void Renderer::BindBufferUnordered(PassHandle pass, ResourceHandle buffer, RHIPipelineStage stage,
                                   StringView bind_point) const
{
    CHECK(mState == State::Setup);
    DeclareBufferAccess(pass, buffer, stage, RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite);
    mSetup->trackedPasses[pass].bufferBindings.emplace_back(buffer, RHIDescriptorType::StorageBuffer, bind_point);
    mSetup->bindingCounts[RHIDescriptorType::StorageBuffer]++;
}
void Renderer::BindBufferShaderRead(PassHandle pass, ResourceHandle buffer, RHIPipelineStage stage) const
{
    CHECK(mState == State::Setup);
    DeclareBufferAccess(pass, buffer, stage, RHIResourceAccessBits::ShaderRead);
}
void Renderer::BindBufferCopyDst(PassHandle pass, ResourceHandle buffer) const
{
    CHECK(mState == State::Setup);
    DeclareBufferAccess(pass, buffer, RHIPipelineStageBits::Transfer, RHIResourceAccessBits::TransferWrite);
}
void Renderer::BindBufferCopySrc(PassHandle pass, ResourceHandle buffer) const
{
    CHECK(mState == State::Setup);
    DeclareBufferAccess(pass, buffer, RHIPipelineStageBits::Transfer, RHIResourceAccessBits::TransferRead);
}
void Renderer::BindTextureSampler(PassHandle pass, ResourceHandle sampler, StringView bind_point) const
{
    CHECK(mState == State::Setup);
    mSetup->trackedPasses[pass].samplers.emplace_back(sampler, bind_point);
    mSetup->bindingCounts[RHIDescriptorType::Sampler]++;
}
void Renderer::BindDescriptorSet(PassHandle pass, StringView bind_point, RHIDeviceDescriptorSet* descriptor_set,
                                 RHIDeviceDescriptorSetLayout* layout)
{
    CHECK(mState == State::Setup);
    mSetup->trackedPasses[pass].externalBindings.emplace_back(descriptor_set, layout, bind_point);
}
void Renderer::BindDescriptorBindPoint(PassHandle pass, StringView bind_point, uint32_t binding, uint32_t set)
{
    CHECK(mState == State::Setup);
    mSetup->trackedPasses[pass].explictDescriptorBindings.emplace_back(binding, set, bind_point);
}
ResourceHandle Renderer::BindTextureSRV(PassHandle pass, ResourceHandle texture, StringView bind_point,
                                        RHIPipelineStage stage, RHITextureViewDesc const& desc) const
{
    CHECK(mState == State::Setup);
    CHECK_MSG(desc.range.IsValid(), "Binding SRV on {} is of invalid range! Did you specify `desc.range`?",
              mSetup->trackedResources[texture].name);
    DeclareTextureAccess(pass, texture, stage, desc.range, RHIResourceAccessBits::ShaderRead,
                         RHITextureLayout::ShaderReadOnly);
    ResourceHandle view = CreateTextureView(pass, texture, desc);
    mSetup->trackedPasses[pass].textureBindings.emplace_back(view, RHIDescriptorType::SampledImage, bind_point);
    mSetup->bindingCounts[RHIDescriptorType::SampledImage]++;
    return view;
}
ResourceHandle Renderer::BindTextureUAV(PassHandle pass, ResourceHandle texture, StringView bind_point,
                                        RHIPipelineStage stage, RHITextureViewDesc const& desc) const
{
    CHECK(mState == State::Setup);
    CHECK_MSG(desc.range.IsValid(), "Binding UAV on {} is of invalid range! Did you specify `desc.range`?",
              mSetup->trackedResources[texture].name);
    DeclareTextureAccess(pass, texture, stage, desc.range,
                         RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite,
                         RHITextureLayout::General);
    ResourceHandle view = CreateTextureView(pass, texture, desc);
    mSetup->trackedPasses[pass].textureBindings.emplace_back(view, RHIDescriptorType::StorageImage, bind_point);
    mSetup->bindingCounts[RHIDescriptorType::StorageImage]++;
    return view;
}
ResourceHandle Renderer::BindTextureRTV(PassHandle pass, ResourceHandle texture, RHITextureViewDesc const& desc,
                                        RHIPipelineState::PipelineStateDesc::Attachment::Blending const& blending) const
{
    CHECK(mState == State::Setup);
    CHECK_MSG(desc.range.IsValid(), "Binding RTV on {} is of invalid range! Did you specify `desc.range`?",
              mSetup->trackedResources[texture].name);
    auto& tpass = mSetup->trackedPasses[pass];
    CHECK_MSG(tpass.queue == RHIDeviceQueueType::Graphics,
              "RTV (Render Target Views) are only supported on Graphics queues");
    RHITextureAspectFlag kRTVBits = RHITextureAspectFlagBits::Color;
    CHECK_MSG(((desc.range.layer.aspect | kRTVBits) == kRTVBits) && (desc.range.layer.aspect & kRTVBits),
              "RTV view must have exactly one layer, and the access flag must be Color.");
    DeclareTextureAccess(pass, texture, RHIPipelineStageBits::RenderTargetOutput, desc.range,
                         RHIResourceAccessBits::RenderTargetWrite, RHITextureLayout::RenderTarget);
    ResourceHandle view = CreateTextureView(pass, texture, desc);
    tpass.rtvs.emplace_back(view, blending);
    return view;
}
ResourceHandle Renderer::BindTextureDSV(PassHandle pass, ResourceHandle texture, RHITextureViewDesc const& desc) const
{
    CHECK(mState == State::Setup);
    CHECK_MSG(desc.range.IsValid(), "Binding DSV on {} is of invalid range! Did you specify `desc.range`?",
              mSetup->trackedResources[texture].name);
    auto& tpass = mSetup->trackedPasses[pass];
    CHECK_MSG(tpass.queue == RHIDeviceQueueType::Graphics,
              "DSV (Depth Stencil Views) are only supported on Graphics queues");
    RHITextureAspectFlag kDSVBits = RHITextureAspectFlagBits::Depth | RHITextureAspectFlagBits::Stencil;
    CHECK_MSG(((desc.range.layer.aspect | kDSVBits) == kDSVBits) && (desc.range.layer.aspect & kDSVBits),
              "DSV view must have exactly one layer, and the access flag must be Depth and/or Stencil.");
    DeclareTextureAccess(pass, texture,
                         RHIPipelineStageBits::EarlyFragmentTests | RHIPipelineStageBits::LateFragmentTests, desc.range,
                         RHIResourceAccessBits::DepthStencilRead | RHIResourceAccessBits::DepthStencilWrite,
                         RHITextureLayout::DepthStencil);
    ResourceHandle view = CreateTextureView(pass, texture, desc);
    tpass.dsv = view;
    return view;
}
void Renderer::BindBackbufferRTV(PassHandle pass,
                                 RHIPipelineState::PipelineStateDesc::Attachment::Blending const& blending) const
{
    CHECK(mState == State::Setup);
    auto& tpass = mSetup->trackedPasses[pass];
    CHECK_MSG(tpass.queue == RHIDeviceQueueType::Graphics,
              "RTV (Render Target Views) are only supported on Graphics queues");
    tpass.writeBackbuffer = true;
    tpass.writeBackbufferBlending = blending;
    if (mSetup->lastBackbufferProducer != kInvalidHandle && mSetup->lastBackbufferProducer != pass)
        mSetup->add_edge(pass, mSetup->lastBackbufferProducer, kInvalidHandle);
    mSetup->lastBackbufferProducer = pass;
}
void Renderer::BindTextureCopyDst(PassHandle pass, ResourceHandle texture,
                                  RHITextureSubresourceRange const& range) const
{
    CHECK(mState == State::Setup);
    CHECK_MSG(range.IsValid(), "Binding CopyDst on {} is of invalid range! Did you specify `desc.range`?",
              mSetup->trackedResources[texture].name);
    DeclareTextureAccess(pass, texture, RHIPipelineStageBits::Transfer, range, RHIResourceAccessBits::TransferWrite,
                         RHITextureLayout::TransferDst);
}
void Renderer::BindTextureCopySrc(PassHandle pass, ResourceHandle texture,
                                  RHITextureSubresourceRange const& range) const
{
    CHECK(mState == State::Setup);
    CHECK_MSG(range.IsValid(), "Binding CopySrc on {} is of invalid range! Did you specify `desc.range`?",
              mSetup->trackedResources[texture].name);
    DeclareTextureAccess(pass, texture, RHIPipelineStageBits::Transfer, range, RHIResourceAccessBits::TransferRead,
                         RHITextureLayout::TransferSrc);
}
void Renderer::PassSetRasterizerFlags(PassHandle pass,
                                      RHIPipelineState::PipelineStateDesc::Rasterizer const& rasterizer,
                                      RHIPipelineState::PipelineStateDesc::DepthStencil const& depth_stencil) const
{
    CHECK(mState == State::Setup);
    auto& tpass = mSetup->trackedPasses[pass];
    tpass.psoRasterizer = rasterizer;
    tpass.psoDepthStencil = depth_stencil;
}
/* --- */
void Renderer::EndSetup()
{
    CHECK_MSG(mState == State::Setup, "Bad renderer state ({}). Did you call BeginSetup()?", mState);
    if (!mSetup->trackedPasses.empty())
    {
        // Setup all passes
        for (auto& pass : mSetup->trackedPasses)
        {
            pass.pass->Setup(pass.handle, this);
        }
        CullPasses(mSetup->epilogue);
        FinalizeResources();
        FinalizePSOs();
    }
    else
    {
        LOG_RUNTIME(Renderer, warn, "No passes created in render graph.");
    }
    mState = State::PostSetup;
}
void Renderer::CullPasses(PassHandle epilogue) const
{
    CHECK(mState == State::Setup);
    CHECK(epilogue < mSetup->trackedPasses.size());
    // Cull and topsort
    Vector<PassHandle> topo(mAllocator), vis(mSetup->trackedPasses.size(), mAllocator),
        dis(mSetup->trackedPasses.size(), mAllocator); // Depth in graph from epilogue
    topo.reserve(mSetup->trackedPasses.size());
    auto dp = [&](PassHandle u, PassHandle pa, auto&& dfs) -> void
    {
        if (u >= mSetup->graph.size())
            return; // No out degrees
        vis[u] = 1;
        for (const auto& v : mSetup->graph[u] | Views::keys)
        {
            // Weighted by vertex priority
            size_t w = 1 + mSetup->trackedPasses[v].priority;
            dis[v] = std::max(dis[u] + w, dis[v]);
            if (vis[v] == 1)
                throw std::runtime_error("Cycle detected in render graph");
            if (vis[v] == 0)
                dfs(v, u, dfs);
        }
        vis[u] = 2;
        mSetup->trackedPasses[u].used = true;
        topo.push_back(u);
    };
    auto& exec = mSetup->execution;
    if (!mSetup->graph.empty())
    {
        dp(epilogue, -1, dp);
        // Sort by longest path
        // Ordering is still valid topological order
        Ranges::sort(topo, [&](auto const& a, auto const& b) { return dis[a] > dis[b]; });
        exec = topo;
    }
    if (exec.empty())
    {
        // No dependency from any passes
        // Execute only the epilogue
        exec.push_back(epilogue);
        mSetup->trackedPasses[epilogue].used = true;
    }
    mSetup->epilogue = epilogue;
    // Collect active resources
    for (PassHandle ord = 0; ord < exec.size(); ord++)
    {
        auto& pass = mSetup->trackedPasses[exec[ord]];
        // Derive lifetimes for resources from execution order
        // FinalizeResources() uses this to overlap resources.
        pass.ord = ord, pass.depth = dis[pass.handle];
        auto& resources = pass.resources;
        // Sort then make unique
        Ranges::sort(resources);
        resources.erase(Ranges::unique(resources).begin(), resources.end());
        for (auto res : resources)
        {
            auto& tres = mSetup->trackedResources[res];
            if (pass.queue == RHIDeviceQueueType::Graphics)
                tres.hasGraphicsUsage = true;
            if (pass.queue == RHIDeviceQueueType::Compute)
                tres.hasComputeUsage = true;
            if (!mSetup->activeResources.contains(res))
                mSetup->activeResources[res] = {ord, ord};
            else
            {
                auto& [t_min, t_max] = mSetup->activeResources[res];
                t_min = std::min(t_min, ord);
                t_max = std::max(t_max, ord);
            }
        }
    }
    // Reorder passes within the same depth level to their relative insertion order (i.e. handle values)
    for (PassHandle i = 0, j = 0; i < exec.size(); i = j)
    {
        while (j < exec.size() && mSetup->trackedPasses[exec[j]].depth == mSetup->trackedPasses[exec[i]].depth)
            j++;
        Ranges::sort(exec.begin() + i, exec.begin() + j, [&](PassHandle a, PassHandle b)
                     { return mSetup->trackedPasses[a].handle < mSetup->trackedPasses[b].handle; });
    }
    auto& exec_group = mSetup->executionGroups;
    // Grouping heuristics:
    // 1: Group contains only passes of same queue types
    // 2: Graphics passes that don't depend on prior Compute separates ones that do while satisfying (1)
    Set<ResourceHandle> produced(mAllocator); // Coarse, ignores sub resources
    auto noDependenciesProduced = [&](PassHandle pass)
    {
        auto const& tpass = mSetup->trackedPasses[pass];
        return Ranges::none_of(tpass.resources, [&](auto const& r) { return produced.contains(r); });
    };
    auto pushDependenciesProduced = [&](PassHandle pass)
    {
        auto const& tpass = mSetup->trackedPasses[pass];
        auto textureProduces = Views::all(tpass.textureUsages) |
            Views::filter([](auto const& t) { return (std::get<1>(t) & kAllShaderWrites); }) | Views::keys;
        auto bufferProduces = Views::all(tpass.bufferUsages) |
            Views::filter([](auto const& b) { return (std::get<1>(b) & kAllShaderWrites); }) | Views::keys;
        produced.insert(textureProduces.begin(), textureProduces.end());
        produced.insert(bufferProduces.begin(), bufferProduces.end());
    };
    for (PassHandle i = 0, j = 0; i < exec.size(); i = j)
    {
        while (j < exec.size() && mSetup->trackedPasses[exec[j]].queue == mSetup->trackedPasses[exec[i]].queue)
        {
            auto queue = mSetup->trackedPasses[exec[j]].queue;
            if (queue == RHIDeviceQueueType::Compute)
                pushDependenciesProduced(exec[j]);
            PassHandle next = (j + 1) < exec.size() ? exec[j + 1] : kInvalidHandle;
            bool nextProducedByCompute = next != kInvalidHandle && !noDependenciesProduced(next);
            bool currentProducedByCompute = !noDependenciesProduced(exec[j]);
            j++;
            if (queue == RHIDeviceQueueType::Graphics && nextProducedByCompute && !currentProducedByCompute)
                break; // Start new group
        }
        auto& group = exec_group.emplace_back(static_cast<int>(exec_group.size()), mSetup->trackedPasses[exec[i]].queue,
                                              mAllocator);
        group.passes.insert(group.passes.end(), exec.begin() + i, exec.begin() + j);
        // Collect dependencies
        for (auto pass : exec_group.back().passes)
        {
            auto& tpass = mSetup->trackedPasses[pass];
            tpass.groupIndex = group.groupIndex;
            group.resources.insert(group.resources.end(), tpass.resources.begin(), tpass.resources.end());
            group.allStages |= tpass.piplineStages;
        }
        // Sort and unique
        Ranges::sort(group.resources);
        group.resources.erase(Ranges::unique(group.resources).begin(), group.resources.end());
        if (group.queue == RHIDeviceQueueType::Graphics)
            mSetup->executionAnyGraphics = true;
        else if (group.queue == RHIDeviceQueueType::Compute)
            mSetup->executionAnyCompute = true;
    }
    // Assign last Graphics/Compute group
    {
        auto it = Ranges::find_if(mSetup->executionGroups | Views::reverse,
                                  [](auto const& g) { return g.queue == RHIDeviceQueueType::Graphics; });
        if (it != mSetup->executionGroups.rend())
            it->isLastGraphics = true;
        it = Ranges::find_if(mSetup->executionGroups | Views::reverse,
                             [](auto const& g) { return g.queue == RHIDeviceQueueType::Compute; });
        if (it != mSetup->executionGroups.rend())
            it->isLastCompute = true;
    }
    // Assign graphics/compute group indices
    {
        size_t graphics_count = 0, compute_count = 0;
        for (auto& g : mSetup->executionGroups)
        {
            if (g.queue == RHIDeviceQueueType::Graphics)
                g.graphicsGroupIndex = graphics_count++;
            else if (g.queue == RHIDeviceQueueType::Compute)
                g.computeGroupIndex = compute_count++;
        }
    }
    LOG_RUNTIME(Renderer, debug, "** Render Graph GraphViz **\n{}", DbgDumpGraphviz());
    LOG_RUNTIME(Renderer, debug, "** Render Graph Execution Order **\n{}", DbgDumpActivePasses());
    LOG_RUNTIME(Renderer, debug, "** Render Graph Execution Groups **\n{}", DbgDumpExecutionGroups());
}
/* -- PSO -- */
void Renderer::BuildPipelineState(PassHandle pass)
{
    auto& tracked = mSetup->trackedPasses[pass];
    ZoneScoped;
    ZoneNameF("Build %s PSO", tracked.name.c_str());
    Vector<RHIPipelineState::PipelineStateDesc::ShaderStage> pso_stages(mAllocator);
    // Load shader bytecode
    if (tracked.shaders.empty())
        return; // Pass with no shaders
    LOG_RUNTIME(Renderer, info, "** Building PSO for {} [{}] **", tracked.name, pass);
    Vector<char> data(mAllocator);
    Map<Native::Path, RHIDeviceScopedObjectHandle<RHIShaderModule>> shaders(mAllocator);
    Map<Native::Path, UniquePtr<Shader>> reflections(mAllocator);
    for (auto const& [shader_path, entry_point, stage] : tracked.shaders)
    {
        if (!shaders.contains(shader_path))
        {
            LOG_RUNTIME(Renderer, debug, "Loading shader {}", shader_path);
            Native::ReadFile(shader_path, data);
            reflections.emplace(shader_path, ConstructUnique<Shader>(mAllocator, data, mAllocator));
            shaders[shader_path] = mDevice->CreateShaderModule({.source = data});
            shaders[shader_path]->DebugSetObjectName(shader_path.string().c_str());
        }
        auto& module = shaders[shader_path];
        // In BindShader we have already guaranteed these to be unique per stage
        if (stage == RHIShaderStageBits::Compute)
            tracked.isComputePass = true;
        bool found = false;
        for (auto const& ep : reflections[shader_path]->mEntrypoints)
        {
            if (ep.stage == stage && ep.name == entry_point)
            {
                pso_stages.push_back(
                    {.desc = {.stage = stage, .entryPoint = ep.name.c_str()}, .shaderModule = module.Get()});
                if (stage & (RHIShaderStageBits::Compute | RHIShaderStageBits::Mesh | RHIShaderStageBits::Task))
                    tracked.groupLocalSize = ep.groupLocalSize;
                found = true;
                break;
            }
        }
        CHECK_MSG(found, "No entry point {} found for stage {} in shader {}", entry_point, stage, shader_path);
    }
    if (tracked.isComputePass)
    {
        CHECK_MSG(shaders.size() == 1,
                  "Pass {} must have exactly 1 Compute Shader, and 0 of any other types, if CS is used.", tracked.name);
        CHECK_MSG(tracked.writeBackbuffer == false, "Pass {} uses Compute Shader, and cannot write to the backbuffer.",
                  tracked.name);
        CHECK_MSG(tracked.rtvs.empty() && tracked.dsv == kInvalidHandle,
                  "Pass {} uses Compute Shader, and cannot have RTVs or DSVs.", tracked.name);
    }
    // Check variable bindings to be consistent across stages
    // [name, [set, binding]]
    Map<String, Pair<uint32_t, uint32_t>> refl_var_bind_points(mAllocator);
    // Push explict declarations first
    for (auto const& [binding, set, name] : tracked.explictDescriptorBindings)
    {
        auto it = refl_var_bind_points.find(name);
        if (it == refl_var_bind_points.end())
            refl_var_bind_points[name] = {set, binding};
        else
        {
            auto& [set_prev, binding_prev] = it->second;
            CHECK_MSG(set_prev == set && binding_prev == binding,
                      "Inconsistent explicit binding points across shader stages for variable {} in pass {}", name,
                      tracked.name);
        }
    }
    // Check if any shader in the pipeline uses PC
    for (auto const& [path, refl] : reflections)
    {
        if (!refl->mPushConstants.empty())
        {
            CHECK_MSG(refl->mPushConstants.size() == 1,
                      "Shader uses more than Push Constant block. This is not accepted by most drivers.");
            CHECK_MSG(
                !tracked.pushConstants.empty(),
                "Shader {} uses Push Constant, but no Push Constant is bound in pass {}. Did you forget to bind it?",
                path, tracked.name);
        }
        for (auto& bind : refl->mBindings)
        {
            CHECK_MSG(!bind.name.empty(), "Unnamed bindings are not supported. Enable debug information for shader {}",
                      path);
            auto it = refl_var_bind_points.find(bind.name);
            if (it == refl_var_bind_points.end())
                refl_var_bind_points[bind.name] = {bind.descriptorSet, bind.binding};
            else
            {
                auto& [set, binding] = it->second;
                CHECK_MSG(set == bind.descriptorSet && binding == bind.binding,
                          "Inconsistent binding points across shader stages for variable {} in shader {}", bind.name,
                          path);
            }
        }
    }
    // Create descriptor set layout to be consistent across stages
    Map<String, RHIDescriptorType> var_types(mAllocator);
    Map<String, ResourceHandle> var_handles(mAllocator);
    Map<String, ResourceHandle> var_samplers(mAllocator);
    Map<String, RHIDeviceDescriptorSet*> var_ext_sets(mAllocator);
    // Textures
    for (auto& [vhdl, dtype, binding] : tracked.textureBindings)
    {
        auto it = var_types.find(binding);
        if (it == var_types.end())
            var_types[binding] = dtype, var_handles[binding] = vhdl;
        else
        {
            auto& dtype_prev = it->second;
            auto& vhdl_prev = var_handles[binding];
            CHECK(dtype_prev == dtype && vhdl_prev == vhdl);
        }
    }
    // Buffers
    for (auto& [rhdl, dtype, binding] : tracked.bufferBindings)
    {
        auto it = var_types.find(binding);
        if (it == var_types.end())
            var_types[binding] = dtype, var_handles[binding] = rhdl;
        else
        {
            auto& dtype_prev = it->second;
            auto& rhdl_prev = var_handles[binding];
            CHECK(dtype_prev == dtype && rhdl_prev == rhdl);
        }
    }
    // Samplers
    for (auto& [sampler_handle, binding] : tracked.samplers)
    {
        auto it = var_types.find(binding);
        if (it == var_types.end())
            var_types[binding] = RHIDescriptorType::Sampler, var_samplers[binding] = sampler_handle;
        else
        {
            auto& dtype_prev = it->second;
            auto& var_handle = var_samplers[binding];
            CHECK(dtype_prev == RHIDescriptorType::Sampler && var_handle == sampler_handle);
        }
    }
    // External sets (e.g. @ref TexturePool)
    for (auto& [desc_set, desc_set_layout, binding] : tracked.externalBindings)
    {
        var_ext_sets[binding] = desc_set;
        // We don't create anything for the set - but do resolve these
        // so we can map them later on
        if (refl_var_bind_points.contains(binding))
            tracked.pExternalDescriptorSets.emplace_back(refl_var_bind_points[binding].first, desc_set,
                                                         desc_set_layout);
    }
    Ranges::sort(tracked.pExternalDescriptorSets);
    tracked.pExternalDescriptorSets.erase(Ranges::unique(tracked.pExternalDescriptorSets).begin(),
                                          tracked.pExternalDescriptorSets.end());
    if (!refl_var_bind_points.empty())
    {
        LOG_RUNTIME(Renderer, debug, "Pipeline Parameters");
        for (auto& [name, dtype] : var_types)
        {
            if (!refl_var_bind_points.contains(name))
                continue;
            auto [set, binding] = refl_var_bind_points[name];
            LOG_RUNTIME(Renderer, debug, "\t{}: set {}, binding {}, type {}", name, set, binding, dtype);
        }
    }
    // [[set, binding], name]
    Vector<Pair<Pair<uint32_t, uint32_t>, String>> bindings(mAllocator);
    bindings.reserve(var_types.size());
    for (auto& [name, bind] : refl_var_bind_points)
    {
        if (!var_ext_sets.contains(name))
            bindings.emplace_back(bind, name);
    }
    Ranges::sort(bindings);
    // Separate into descriptor sets
    Vector<RHIDeviceDescriptorSetLayoutDesc::Binding> set_bindings(mAllocator);
    for (const auto& binding : bindings | Views::values)
    {
        // TODO: Descriptor Arrays?
        // Not currently used by Renderer APIs - and for use cases like bindless,
        // we have @ref BindDescriptorSet to bind a pre-made descriptor set.
        CHECK_MSG(var_types.contains(binding) || var_ext_sets.contains(binding),
                  "Binding {} is not bound by pass {}, but is used by one of its shaders.", binding, tracked.name);
        set_bindings.push_back({.count = 1, .stage = RHIShaderStageBits::All, .type = var_types[binding]});
    }
    // Check if the external set conflicts with our own bindings
    for (auto const& [set, ptr, layout_ptr] : tracked.pExternalDescriptorSets)
    {
        auto it = Ranges::find_if(bindings, [set](auto const& b) { return b.first.first == set; });
        if (it != bindings.end())
        {
            auto e_it =
                Ranges::find_if(tracked.externalBindings, [ptr](auto const& e) { return std::get<0>(e) == ptr; });
            CHECK_MSG(
                false,
                "External descriptor set used by shader at set {} (used by '{}') conflicts with bindings declared by "
                "pass {}, which is declared internally. Declare different set usage _in shader_ for usage!",
                set, std::get<2>(*e_it), tracked.name);
        }
    }
    // Check if our first set is not 0
    if (!bindings.empty() && bindings[0].first.first != 0)
    {
        LOG_RUNTIME(BuildPipelineState, err,
                    "Binding set numbers must start from 0. Error at set {} binding {} in pass {}.",
                    bindings[0].first.first, bindings[0].first.second, tracked.name);
        LOG_RUNTIME(BuildPipelineState, info, kShaderDescriptorBindingErrorHelp);
        CHECK_MSG(false, "Binding set numbers must start from 0.");
    }
    for (uint32_t i = 0, j = 0; i < bindings.size(); i = j)
    {
        uint32_t set = bindings[i].first.first;
        // Check if our first binding is not 0
        if (bindings[i].first.second != 0)
        {
            LOG_RUNTIME(
                BuildPipelineState, err,
                "Binding numbers must start from 0 in each descriptor set. Error at set {} binding {} in pass {}.", set,
                bindings[i].first.second, tracked.name);
            LOG_RUNTIME(BuildPipelineState, info, kShaderDescriptorBindingErrorHelp);
            CHECK_MSG(false, "Binding binding numbers must start from 0.");
        }
        while (j < bindings.size() && bindings[j].first.first == set)
            j++;
        // Check and create descriptors
        // In short - we ensure that there'd be no undefined access from the shaders
        // thus all _shader_ bindings are guaranteed to be bound
        // We do not check if _all_ bound resources are used by shaders - unused, unbound
        // resources are allowed.
        tracked.descriptorLayouts.push_back(
            mDevice->CreateDescriptorSetLayout({.bindings = {set_bindings.cbegin() + i, set_bindings.cbegin() + j}}));
        tracked.descriptorLayouts.back()->DebugSetObjectName(
            fmt::format("Descriptor Set Layout {} of {} [{}]", set, tracked.name, pass).c_str());
        tracked.pDescriptorLayouts.emplace_back(tracked.descriptorLayouts.back().Get());
        {
            std::unique_lock lock(mDescPoolMutex);
            tracked.descriptorSets.push_back(mDescPool->CreateDescriptorSet(tracked.descriptorLayouts.back()));
        }
        auto& ds = tracked.descriptorSets.back();
        ds->DebugSetObjectName(fmt::format("Descriptor Set {} of {} [{}]", set, tracked.name, pass).c_str());
        tracked.pDescriptorSets.push_back(ds.Get());
        // Update bindings
        LOG_RUNTIME(Renderer, debug, "Descriptor Set {} Bindings", set);
        for (size_t k = i; k < j; k++)
        {
            auto const& [bind, name] = bindings[k];
            auto const& [binding_set, binding] = bind;
            auto const& hdl = var_handles[name];
            CHECK_MSG(var_types.contains(name),
                      "Binding {} is undefined in pass {}, but referenced by one of its shaders", name, tracked.name);
            auto const& type = var_types[name];
            using enum RHIDescriptorType;
            switch (type)
            {
            case Sampler:
                {
                    CHECK_MSG(var_samplers.contains(name),
                              "Shader expects a Sampler at {}, but it's not bound by pass {}", name, tracked.name);
                    auto& sampler_handle = var_samplers[name];
                    auto* sampler = DerefSampler(sampler_handle);
                    LOG_RUNTIME(Renderer, debug, "\t[Sampler] {}: binding {}, type {}", name, binding, type);
                    ds->Update({.binding = binding, .type = type, .images = {{{.sampler = sampler}}}});
                    break;
                }
            case SampledImage:
            case StorageImage:
                {
                    auto* view = DerefTextureView(hdl);
                    LOG_RUNTIME(Renderer, debug, "\t[Texture] {}: binding {}, type {}", name, binding, type);
                    ds->Update({.binding = binding,
                                .type = type,
                                .images = {{{.imageView = view,
                                             .layout = type == RHIDescriptorType::SampledImage
                                                 ? RHITextureLayout::ShaderReadOnly
                                                 : RHITextureLayout::General}}}});
                    break;
                }
            case UniformBuffer:
            case StorageBuffer:
                {
                    LOG_RUNTIME(Renderer, debug, "\t[Buffer] {}: binding {}, type {}", name, binding, type);
                    auto* buf = DerefResource(hdl).Get<RHIBuffer*>();
                    ds->Update({.binding = binding, .type = type, .buffers = {{{.buffer = buf}}}});
                    break;
                }
            default:
                break;
            }
        }
    }
    // Add external sets
    // We've already established that these would not conflict, and has already been sorted
    for (auto const& [set, ptr, layout_ptr] : tracked.pExternalDescriptorSets)
    {
        tracked.pDescriptorSets.emplace_back(ptr);
        tracked.pDescriptorLayouts.emplace_back(layout_ptr);
    }
    // Create PSO if we have shader stages
    if (!pso_stages.empty())
    {
        RHIPipelineState::PipelineStateDesc pso_desc{
            .type = tracked.isComputePass ? RHIDevicePipelineType::Compute : RHIDevicePipelineType::Graphics,
            .vertexInput = {.bindings = tracked.vertexInputBindings, .attributes = tracked.vertexInputAttributes},
            .topology = RHIPipelineState::PipelineStateDesc::TriangleList,
            .rasterizer = tracked.psoRasterizer,
            .multisample = {.enabled = false},
            .depthStencil = tracked.psoDepthStencil,
            .shaderStages = pso_stages,
            .descriptorSetLayouts = tracked.pDescriptorLayouts,
            .pushConstants = tracked.pushConstants};
        // Setup compute/graphics specific states
        // Graphics
        // RTV,DSV
        Vector<RHIPipelineState::PipelineStateDesc::Attachment> attachments(mAllocator);
        if (tracked.writeBackbuffer)
        {
            CHECK_MSG(tracked.rtvs.empty(), "Pass {} writes to backbuffer, and cannot have other RTVs.", tracked.name);
            // Only write to the backbuffer
            attachments.push_back(
                {.blending = tracked.writeBackbufferBlending, .renderTarget = {.format = mSwapchain->mDesc.format}});
        }
        else
        {
            for (auto const& [rtv, blending] : tracked.rtvs)
            {
                auto& [rhdl, desc] = mSetup->trackedViews[rtv];
                attachments.push_back({.blending = blending, .renderTarget = {.format = desc.format}});
            }
        }
        pso_desc.attachments = attachments;
        pso_desc.depthStencil = {
            .depthTest = tracked.dsv != kInvalidHandle,
            .depthWrite = tracked.dsv != kInvalidHandle,
            .depthCompareOp = RHIPipelineState::PipelineStateDesc::DepthStencil::CompareOp::Less,
        };
        if (tracked.dsv != kInvalidHandle)
        {
            auto& [dsv_handle, desc] = mSetup->trackedViews[tracked.dsv];
            pso_desc.depthStencil.depthFormat = desc.format;
            // TODO Stencil?
        }
        tracked.pso = mDevice->CreatePipelineState(pso_desc);
        tracked.pso->DebugSetObjectName(fmt::format("PSO of {} [{}]", tracked.name, pass).c_str());
    }
    else
    {
        LOG_RUNTIME(Renderer, debug, "Pass {} has no shader stages, and thus no PSO is created.", tracked.name);
    }
}
void Renderer::FinalizePSOs()
{
    CHECK(mState == State::Setup);
    // Build descriptor pool
    mDescPool.Reset();
    if (!mSetup->bindingCounts.empty())
    {
        Vector<RHIDeviceDescriptorPool::PoolDesc::Binding> bindings(mAllocator);
        bindings.reserve(mSetup->bindingCounts.size());
        LOG_RUNTIME(Renderer, debug, "** Descriptor Pool **");
        for (auto& [type, count] : mSetup->bindingCounts)
        {
            LOG_RUNTIME(Renderer, debug, "\t{}: {}", type, count);
            bindings.push_back({.type = type, .maxCount = count});
        }
        mDescPool = mDevice->CreateDescriptorPool({bindings});
        mDescPool->DebugSetObjectName("Renderer Descriptor Pool");
    }
    // Build PSOs for everything we need
    LOG_RUNTIME(Renderer, info, "Compiling Shaders");
    Async::ThreadPool pool(std::thread::hardware_concurrency(), kMaxRenderPasses, mAllocator, "PSOComp");
    Vector<Async::SharedPromise<void>> futures(mAllocator);
    for (auto& pass : mSetup->trackedPasses)
    {
        if (!pass.used)
            continue;
        futures.emplace_back(pool.Push([&] { BuildPipelineState(pass.handle); }));
    }
    for (size_t i = 0; i < futures.size(); i++)
    {
        auto& tpass = mSetup->trackedPasses[i];
        try
        {
            futures[i]->get_future().get();
        } catch (std::runtime_error const& e)
        {
            LOG_RUNTIME(Renderer, err, "Failed to build PSO for pass {}: {}", tpass.name, e.what());
            throw; // Failfast
        }
    }
    LOG_RUNTIME(Renderer, info, "Compiled Shaders.");
}
void Renderer::FinalizeResources()
{
    CHECK(mState == State::Setup);
    mResources = ConstructUnique<ExecuteResources>(mAllocator, mAllocator);
    if (!mSetup->activeResources.empty())
        mResources->fit(mSetup->trackedResources.size());
    // !! TODO: Overlap transient resources if possible
    for (const auto& handle : mSetup->activeResources | Views::keys)
    {
        auto& res = mSetup->trackedResources[handle];
        res.desc.Visit(
            // Owned
            [&](RHIBufferDesc const& desc)
            {
                mResources->resources[handle] = mDevice->CreateBuffer(desc);
                DerefResource(handle).Get<RHIBuffer*>()->DebugSetObjectName(
                    fmt::format("{} [{}]", res.name, handle).c_str());
            },
            [&](RHITextureDesc const& desc)
            {
                mResources->resources[handle] = mDevice->CreateTexture(desc);
                DerefResource(handle).Get<RHITexture*>()->DebugSetObjectName(
                    fmt::format("{} [{}]", res.name, handle).c_str());
            },
            // Borrowed
            [&](RHIDeviceObjectHandle<RHIBuffer> const& hdl) { mResources->resources[handle] = hdl; },
            [&](RHIDeviceObjectHandle<RHITexture> const& hdl) { mResources->resources[handle] = hdl; },
            [&](RHIBuffer* const ptr) { mResources->resources[handle] = ptr; },
            [&](RHITexture* const ptr) { mResources->resources[handle] = ptr; },
            [&](auto const&) { throw std::runtime_error("Unhandled resource type at creation time"); });
    }
    // Add back buffers (if we need to present)
    if (mDesc.present)
    {
        for (size_t i = 0; i < mFrameSwaps; i++)
        {
            ResourceHandle handle = mSwaps[i].backbuffer;
            auto& tres = mSetup->trackedResources[handle];
            mResources->resources[handle] = tres.desc.Get<RHITexture*>();
        }
    }
    // Create texture views
    Vector<ResourceHandle> activeViews(mAllocator), activeSamplers(mAllocator);
    for (PassHandle ord = 0; ord < mSetup->execution.size(); ord++)
    {
        auto& pass = mSetup->trackedPasses[mSetup->execution[ord]];
        for (auto hdl : pass.texviews)
            activeViews.push_back(hdl);
        for (const auto& key : pass.samplers | Views::keys)
            activeSamplers.push_back(key);
    }
    // Instantiate views
    Ranges::sort(activeViews);
    activeViews.erase(Ranges::unique(activeViews).begin(), activeViews.end());
    if (!activeViews.empty())
        mResources->fit(Ranges::max(activeViews));
    for (auto hdl : activeViews)
    {
        auto [rhdl, desc] = mSetup->trackedViews[hdl];
        auto res = DerefResource(rhdl).Get<RHITexture*>();
        mResources->views[hdl] = res->CreateTextureView(desc);
    }
    // Instantiate samplers
    Ranges::sort(activeSamplers);
    activeSamplers.erase(Ranges::unique(activeSamplers).begin(), activeSamplers.end());
    if (!activeSamplers.empty())
        mResources->fit(Ranges::max(activeSamplers));
    for (auto hdl : activeSamplers)
    {
        auto& desc = mSetup->trackedSamplers[hdl];
        mResources->samplers[hdl] = mDevice->CreateSampler(desc);
    }
    // Reset resource states
    for (auto& res : mSetup->trackedResources)
    {
        res.lastBufferState.reset();
        for (auto& sta : res.lastSubresourceStates)
            sta.reset();
    }
}
void Renderer::SetFrameSyncObjects()
{
    while (mSwaps.size() < mFrameSwaps)
        mSwaps.emplace_back(mSwaps.size());
    while (mExecutePerSwapCmds.size() < mFrameSwaps)
    {
        auto& threads = mExecutePerSwapCmds.emplace_back(mAllocator);
        while (threads.size() < mDesc.renderThreads + 1) // Inc. main (render) thread. We do work too!
            threads.emplace_back(ConstructUnique<ExecutePerThreadCommandLists>(mAllocator, mDevice.Get(),
                                                                               kMaxCommandListsPerThread, mAllocator));
    }
    for (size_t i = 0; i < mFrameSwaps; i++)
    {
        mSwaps[i].render = mDevice->CreateSemaphore(false);
        mSwaps[i].render->DebugSetObjectName(fmt::format("Render Semaphore of Swap {}", i).c_str());
        mSwaps[i].present = mDevice->CreateSemaphore(false);
        mSwaps[i].present->DebugSetObjectName(fmt::format("Present Semaphore of Swap {}", i).c_str());
        mSwaps[i].graphicsFence = mDevice->CreateFence(true);
        mSwaps[i].graphicsFence->DebugSetObjectName(fmt::format("Graphics Fence of Swap {}", i).c_str());
        mSwaps[i].computeFence = mDevice->CreateFence(true);
        mSwaps[i].computeFence->DebugSetObjectName(fmt::format("Compute Fence of Swap {}", i).c_str());
    }
    mGraphicsTimeline = mDevice->CreateSemaphore(true);
    mGraphicsTimeline->DebugSetObjectName(fmt::format("Async Graphics Timeline Semaphore").c_str());
    mComputeTimeline = mDevice->CreateSemaphore(true);
    mComputeTimeline->DebugSetObjectName(fmt::format("Async Compute Timeline Semaphore").c_str());
}
void Renderer::SetSwapchain(RHIDeviceObjectHandle<RHISwapchain> swapchain)
{
    CHECK_MSG(mDesc.present, "Cannot set swapchain when the renderer is not declared with Present support");
    mFrameSwaps = swapchain->GetImages().size();
    LOG_RUNTIME(Renderer, info, "Swapchain uses {} back buffers", mFrameSwaps);
    if (mState == State::Execute)
    {
        // If changing swapchain during execution (e.g. due to resize exception)
        // Wait for GPU to be idle
        LOG_RUNTIME(Renderer, info, "Swapchain is already in execute??");
        mDevice->WaitIdle();
        mState = State::PostSetup;
    }
    SetFrameSyncObjects();
    for (size_t i = 0; i < mFrameSwaps; ++i)
    {
        auto* backbuffer = swapchain->GetImages()[i];
        backbuffer->DebugSetObjectName(fmt::format("Backbuffer of Swap {}", i).c_str());
        mSwaps[i].rtv = backbuffer->CreateTextureView(
            RHITextureViewDesc{.format = swapchain->mDesc.format, .range = RHITextureSubresourceRange::Create()});
        if (mSwaps[i].backbuffer == kInvalidHandle)
        {
            // First time setup
            mSwaps[i].backbuffer = CreateResource(fmt::format("Backbuffer of Swap {}", i), backbuffer);
        }
        else
        {
            // Update existing handle
            auto& rt_res = mResources->resources[mSwaps[i].backbuffer];
            CHECK_MSG(rt_res.GetIf<RHITexture*>(), "Swapchain backbuffer handle {} is not a texture",
                      mSwaps[i].backbuffer);
            rt_res = backbuffer;
            mSetup->trackedResources[mSwaps[i].backbuffer].ResetStates();
        }
    }
    mSwapchain = swapchain;
    // Reset semaphores index and swapchain frame count
    mFrameSwapped = mCurrentSwap = mCurrentSync = 0;
}
void Renderer::BeginExecute()
{
    CHECK_MSG(mState == State::PostSetup, "Renderer bad state ({}). Did you call EndSetup() or EndExecute()?", mState);
    ZoneScoped;
    mState = State::Execute;
    // Reset per-frame arena
    mExecuteAlloc.Reset(mExecuteArena);
    Vector<RHIDeviceObjectHandle<RHIDeviceFence>> wait_fences(mExecuteAlloc.Ptr());
    if (mSetup->executionAnyGraphics)
        wait_fences.push_back(mSwaps[mCurrentSync].graphicsFence);
    if (mSetup->executionAnyCompute)
        wait_fences.push_back(mSwaps[mCurrentSync].computeFence);
    {
        ZoneScopedN("Wait for GPU");
        mDevice->WaitForFences(wait_fences, true, -1);
        mDevice->ResetFences(wait_fences);
    }
    if (mDesc.present)
    {
        ZoneScopedN("Acquire Next Image");
        mCurrentSwap = mSwapchain->GetNextImage(-1, mSwaps[mCurrentSync].present, {});
    }
    // Reset per-swap command lists
    for (auto& cmds : mExecutePerSwapCmds[mCurrentSync])
        cmds->Reset();
}
void Renderer::ExecuteBarrierSubresourceState(PassHandle pass, RHITexture* res, TrackedResource::SubresourceState& sta,
                                              RHIResourceAccess access, RHIPipelineStage stage, RHITextureLayout layout,
                                              ExecuteBarrierPCmdOrPBarrierList cmd) const
{
    ZoneScoped;
    // RW resources need barriers even if the state doesn't change
    // because of potential hazards
    if ((sta.access & kAllShaderWrites) != 0 || (access & kAllShaderWrites) != 0)
    {
        /* always barrier */
    }
    else if (sta.access == access && sta.stage == stage && sta.layout == layout)
        return;
    RHICommandList::TransitionDesc desc{
        .srcAccess = sta.access,
        .dstAccess = access,
        .srcStage = sta.stage,
        .dstStage = stage,
        .srcImgLayout = sta.layout,
        .dstImgLayout = layout,
        .srcImgRange = sta.ToRange(),
    };
    cmd.Visit([&](RHICommandList* cmdList) { cmdList->SetImageTransition(res, desc); },
              [&](ExecuteBarrierList* barrierList) { barrierList->emplace_back(res, desc); });
    sta.access = access;
    sta.stage = stage;
    sta.layout = layout;
    sta.lastExecutor = pass;
    sta.lastExecuteFrame = mFrameSwapped;
}
void Renderer::ExecuteBarrierSubresource(PassHandle pass, TrackedResource& tres,
                                         RHITextureSubresourceRange const& range, RHIResourceAccess access,
                                         RHIPipelineStage stage, RHITextureLayout layout,
                                         ExecuteBarrierPCmdOrPBarrierList cmd)
{
    ZoneScoped;
    CHECK_MSG(mState == State::Execute, "Renderer bad state ({}). Did you call BeginExecute()?", mState);
    RHITexture* res = DerefResource(tres.handle).Get<RHITexture*>();
    bool any_range = false;
    for (auto& sta : tres.GetLastSubresourceStateOf(range))
    {
        any_range = true;
        ExecuteBarrierSubresourceState(pass, res, sta, access, stage, layout, cmd);
    }
    CHECK_MSG(any_range, "FIXME-ExecuteBarrierSubresource: Failed to match resource range on {}", tres.name);
}
void Renderer::ExecuteBarrierBuffer(PassHandle pass, TrackedResource& tres, RHIResourceAccess access,
                                    RHIPipelineStage stage, ExecuteBarrierPCmdOrPBarrierList cmd)
{
    ZoneScoped;
    CHECK_MSG(mState == State::Execute, "Renderer bad state ({}). Did you call BeginExecute()?", mState);
    RHIBuffer* res = DerefResource(tres.handle).Get<RHIBuffer*>();
    /* Same as textures, RW buffers need barriers even if the state doesn't change */
    if ((tres.lastBufferState.access & kAllShaderWrites) != 0 || (access & kAllShaderWrites) != 0)
    {
        /* always barrier */
    }
    else if (tres.lastBufferState.access == access && tres.lastBufferState.stage == stage)
        return;
    RHICommandList::TransitionDesc desc{
        .srcAccess = tres.lastBufferState.access,
        .dstAccess = access,
        .srcStage = tres.lastBufferState.stage,
        .dstStage = stage,
    };
    cmd.Visit([&](RHICommandList* cmdList) { cmdList->SetBufferTransition(res, desc); },
              [&](ExecuteBarrierList* barrierList) { barrierList->emplace_back(res, desc); });
    tres.lastBufferState.access = access;
    tres.lastBufferState.stage = stage;
    tres.lastBufferState.lastExecutor = pass;
    tres.lastBufferState.lastExecuteFrame = mFrameSwapped;
}
void Renderer::ExecuteBarriers(TrackedPass& pass, ExecuteBarrierPCmdOrPBarrierList cmd)
{
    ZoneScoped;
    CHECK_MSG(mState == State::Execute, "Renderer bad state ({}). Did you call BeginExecute()?", mState);
    // At this point the pass execution order has been determined
    // (execution) and so are the resources' access patterns.
    // Minimal synchronization barriers would always be the most optimal.
    /* -- Textures -- */
    // These are always disjoint ranges
    for (auto [hdl, access, stage, range, layout] : pass.textureUsages)
    {
        auto& tres = mSetup->trackedResources[hdl];
        ExecuteBarrierSubresource(pass.handle, tres, range, access, stage, layout, cmd);
    }
    /* -- Backbuffer -- */
    // A special case with known usages.
    // We never create resource per-swap so tracking Back buffers by passes
    // is not possible. The BB is also opaque to the passes for the same reasons.
    // Synchronization for other resources are already handled above,
    // and should eliminate any redundant per-pass resource creation.
    if (pass.writeBackbuffer)
    {
        CHECK_MSG(pass.queue == RHIDeviceQueueType::Graphics, "Backbuffer can only be used in Graphics queue");
        const RHIResourceAccess rt_access =
            RHIResourceAccessBits::RenderTargetWrite | RHIResourceAccessBits::RenderTargetRead;
        const RHITextureLayout rt_layout = RHITextureLayout::RenderTarget;
        const RHIPipelineStage rt_stage = RHIPipelineStageBits::RenderTargetOutput;
        auto& tres = mSetup->trackedResources[mSwaps[GetSwap()].backbuffer];
        ExecuteBarrierSubresource(pass.handle, tres, RHITextureSubresourceRange::Create(), rt_access, rt_stage,
                                  rt_layout, cmd);
    }
    /* -- Buffers -- */
    // These are always global i.e. at most one per buffer per pass.
    for (auto [hdl, access, stage] : pass.bufferUsages)
    {
        auto& tres = mSetup->trackedResources[hdl];
        ExecuteBarrierBuffer(pass.handle, tres, access, stage, cmd);
    }
}
void Renderer::ExecuteAcquireQueueResources(RHIDeviceQueueType currentQueue, size_t groupIndex, RHICommandList* cmd)
{
    ZoneScoped;
    auto& groups = mSetup->executionGroups;
    if (groups.size() <= 1) // e.g. No Async Compute
        return;
    cmd->BeginTransition();
    uint32_t currentQueueIndex = ExecuteGetQueueIndex(currentQueue);
    for (PassHandle pass : groups[groupIndex].passes)
    {
        auto& tracked = mSetup->trackedPasses[pass];
        for (auto [hdl, access, stage, range, layout] : tracked.textureUsages)
        {
            auto& tres = mSetup->trackedResources[hdl];
            if (!(tres.hasGraphicsUsage && tres.hasComputeUsage))
                continue; // Only care about cross-queue resources
            for (auto& sta : tres.GetLastSubresourceStateOf(range))
            {
                if (sta.lastOwnerQueue == currentQueue)
                    continue;
                if (sta.lastOwnerQueue != RHIDeviceQueueType::Undefined)
                    cmd->SetImageTransition(DerefResource(tres.handle).Get<RHITexture*>(),
                                            {.srcImgRange = sta.ToRange(),
                                             .srcQueueIndex = ExecuteGetQueueIndex(sta.lastOwnerQueue),
                                             .dstQueueIndex = currentQueueIndex});
                sta.lastOwnerQueue = currentQueue;
            }
        }
        for (auto [hdl, access, stage] : tracked.bufferUsages)
        {
            auto& tres = mSetup->trackedResources[hdl];
            if (!(tres.hasGraphicsUsage && tres.hasComputeUsage))
                continue; // Only care about cross-queue resources
            if (tres.lastBufferState.lastOwnerQueue == currentQueue)
                continue;
            if (tres.lastBufferState.lastOwnerQueue != RHIDeviceQueueType::Undefined)
                cmd->SetBufferTransition(DerefResource(tres.handle).Get<RHIBuffer*>(),
                                         {.srcQueueIndex = ExecuteGetQueueIndex(tres.lastBufferState.lastOwnerQueue),
                                          .dstQueueIndex = currentQueueIndex});
            tres.lastBufferState.lastOwnerQueue = currentQueue;
        }
    }
    cmd->EndTransition();
}
void Renderer::ExecuteReleaseQueueResources(RHIDeviceQueueType currentQueue, size_t groupIndex, RHICommandList* cmd)
{
    ZoneScoped;
    auto& groups = mSetup->executionGroups;
    if (groups.size() <= 1) // e.g. No Async Compute
        return;
    /* -- Pre-transition -- */
    // If the _current_ queue is strictly more capable (i.e. Graphics), transition the resources for
    // the _next_ group which is *now* guaranteed to be less capable (i.e. Compute).
    // Only Compute resources _need_ to be transitioned here beforehand. So we only deal with that
    size_t nextGroupIndex = (groupIndex + 1) %
        groups.size(); // Next group. Would be the first group if current groupIndex is the last group
    if (groups[groupIndex].queue == RHIDeviceQueueType::Graphics && groups[nextGroupIndex].queue != currentQueue)
    {
        // Declare that the first pass from the next group handled the transition
        PassHandle executorPass = mSetup->executionGroups[nextGroupIndex].passes.front();
        cmd->BeginTransition();
        for (PassHandle pass : groups[nextGroupIndex].passes)
        {
            auto& tracked = mSetup->trackedPasses[pass];
            for (auto [hdl, access, stage, range, layout] : tracked.textureUsages)
            {
                auto& tres = mSetup->trackedResources[hdl];
                RHITexture* res = DerefResource(tres.handle).Get<RHITexture*>();
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
            }
            for (auto [hdl, access, stage] : tracked.bufferUsages)
            {
                auto& tres = mSetup->trackedResources[hdl];
                // Same as above
                if (tres.lastBufferState.executeTempTransitionFlag)
                    continue;
                ExecuteBarrierBuffer(executorPass, tres, access, stage, cmd);
                tres.lastBufferState.executeTempTransitionFlag = true;
            }
            // Reset the flags
            for (auto [hdl, access, stage, range, layout] : tracked.textureUsages)
            {
                auto& tres = mSetup->trackedResources[hdl];
                for (auto& sta : tres.GetLastSubresourceStateOf(range))
                    sta.executeTempTransitionFlag = false;
            }
            for (auto [hdl, access, stage] : tracked.bufferUsages)
            {
                auto& tres = mSetup->trackedResources[hdl];
                tres.lastBufferState.executeTempTransitionFlag = false;
            }
        }
        cmd->EndTransition();
    }
    else
    { /* Compute - nop */
    }
    // Actually release our resources for subsequent groups
    // Do this for resources to the next queue that may require alternate queue access.
    uint32_t currentQueueIndex = ExecuteGetQueueIndex(currentQueue);
    // Always alternate
    // Not always optimal - but easier than tracking multiple queues across multiple groups across multiple frames
    // The resources that are touched on different queues are usually sparse anyway. And only these are released.
    // Release/Acquire for other resources would be no-ops
    uint32_t nextQueueIndex = ExecuteGetQueueIndex(
        currentQueue == RHIDeviceQueueType::Graphics ? RHIDeviceQueueType::Compute : RHIDeviceQueueType::Graphics);
    cmd->BeginTransition();
    for (PassHandle pass : groups[groupIndex].passes)
    {
        auto& tracked = mSetup->trackedPasses[pass];
        for (auto [hdl, access, stage, range, layout] : tracked.textureUsages)
        {
            auto& tres = mSetup->trackedResources[hdl];
            if (!(tres.hasGraphicsUsage && tres.hasComputeUsage))
                continue; // Only care about cross-queue resources
            for (auto& sta : tres.GetLastSubresourceStateOf(range))
            {
                cmd->SetImageTransition(DerefResource(tres.handle).Get<RHITexture*>(),
                                        {.srcImgRange = sta.ToRange(),
                                         .srcQueueIndex = currentQueueIndex,
                                         .dstQueueIndex = nextQueueIndex});
            }
        }
        for (auto [hdl, access, stage] : tracked.bufferUsages)
        {
            auto& tres = mSetup->trackedResources[hdl];
            if (!(tres.hasGraphicsUsage && tres.hasComputeUsage))
                continue; // Only care about cross-queue resources
            cmd->SetBufferTransition(DerefResource(tres.handle).Get<RHIBuffer*>(),
                                     {.srcQueueIndex = currentQueueIndex, .dstQueueIndex = nextQueueIndex});
        }
    }
    cmd->EndTransition();
}
Renderer::ExecutePerThreadCommandLists::ExecutePerThreadCommandLists(RHIDevice* device, const size_t maxPerThread,
                                                                     Allocator* alloc) :
    graphicsCmds(maxPerThread, alloc), computeCmds(maxPerThread, alloc)
{
    graphicsPool =
        device->CreateCommandPool({.queue = RHIDeviceQueueType::Graphics, .type = RHICommandPoolType::Transient});
    computePool =
        device->CreateCommandPool({.queue = RHIDeviceQueueType::Compute, .type = RHICommandPoolType::Transient});
}
void Renderer::ExecutePerThreadCommandLists::Reset()
{
    graphicsCtr = 0;
    computeCtr = 0;
    graphicsPool->ResetAllCommandLists(false /* freeResources */);
    computePool->ResetAllCommandLists(false /* freeResources */);
}
RHICommandList* Renderer::ExecutePerThreadCommandLists::AllocateGraphics(int thread_id)
{
    size_t index = graphicsCtr++;
    if (!graphicsCmds[index].IsValid())
    {
        graphicsCmds[index] = graphicsPool->CreateCommandList();
        graphicsCmds[index]->DebugSetObjectName(fmt::format("Graphics List {}", index).c_str());
    }
    return graphicsCmds[index].Get();
}
RHICommandList* Renderer::ExecutePerThreadCommandLists::AllocateCompute(int thread_id)
{
    size_t index = computeCtr++;
    if (!computeCmds[index].IsValid())
    {
        computeCmds[index] = computePool->CreateCommandList();
        computeCmds[index]->DebugSetObjectName(fmt::format("Compute List {}", index).c_str());
    }
    return computeCmds[index].Get();
}
RHICommandList* Renderer::ExecuteAllocateCommandList(RHIDeviceQueueType queue, int thread_id)
{
    auto& swap = mExecutePerSwapCmds[mCurrentSync];
    auto& thread = swap[thread_id + 1]; // thread_id == -1 is the main thread
    switch (queue)
    {
    case RHIDeviceQueueType::Compute:
        return thread->AllocateCompute(thread_id + 1);
    default:
    case RHIDeviceQueueType::Graphics:
        return thread->AllocateGraphics(thread_id + 1);
    }
}
void Renderer::ExecuteFrame()
{
    ZoneScoped;
    CHECK_MSG(mState == State::Execute, "Renderer bad state ({}). Did you call BeginExecute()?", mState);
    auto& passes = mSetup->trackedPasses;
    // Execute by groups
    for (auto& group : mSetup->executionGroups)
    {
        /* -- Dependency -- */
        // Collect producers that branched out before this group
        // We only take groups that's produced in this frame before the current one
        int maxGraphicsSyncGroup = -1, maxComputeSyncGroup = -1;
        if (mSetup->executionGroups.size() > 1)
        {
            auto UpdateSyncGroup = [&](PassHandle pass)
            {
                if (pass == kInvalidHandle)
                    return;
                auto& tpass = mSetup->trackedPasses[pass];
                auto& tgroup = mSetup->executionGroups[tpass.groupIndex];
                if (tpass.groupIndex >= group.groupIndex)
                    return;
                switch (tpass.queue)
                {
                case RHIDeviceQueueType::Graphics:
                    maxGraphicsSyncGroup = std::max<int>(maxGraphicsSyncGroup, tgroup.graphicsGroupIndex);
                    break;
                default:
                case RHIDeviceQueueType::Compute:
                    maxComputeSyncGroup = std::max<int>(maxComputeSyncGroup, tgroup.computeGroupIndex);
                }
            };
            for (auto pass_handle : group.passes)
            {
                auto const& pass = mSetup->trackedPasses[pass_handle];
                for (auto [hdl, access, stage, range, layout] : pass.textureUsages)
                {
                    auto& tres = mSetup->trackedResources[hdl];
                    for (auto const& sta : tres.GetLastSubresourceStateOf(range))
                        UpdateSyncGroup(sta.lastExecutor);
                }
                for (auto [hdl, access, stage] : pass.bufferUsages)
                {
                    auto& tres = mSetup->trackedResources[hdl];
                    UpdateSyncGroup(tres.lastBufferState.lastExecutor);
                }
            }
        }
        /* -- Recording -- */
        // Count only the non-skipped ones
        Vector<PassHandle> group_active(mExecuteAlloc.Ptr());
        group_active.reserve(mSetup->executionGroups.size() + 1);
        for (auto handle : group.passes)
        {
            if (!passes[handle].pass->IsSkipped(handle, this))
                group_active.emplace_back(handle);
        }
        // Record all the active tasks
        // We do this in parallel - with transitions starting before the passes
        Vector<ExecuteBarrierList> execute_barriers(mExecuteAlloc.Ptr());
        Vector<RHICommandList*> execute_cmds(mExecuteAlloc.Ptr());
        execute_cmds.resize(group_active.size(), nullptr);
        execute_barriers.resize(group_active.size(), ExecuteBarrierList(mExecuteAlloc.Ptr()));
        {
            // ExecuteBarriers - Set...Barrier calls are very, very cheap and doesn't reach the driver
            // until a call to EndTransition on the cmd
            // This needs to be done in lockstep - helps with cache locality as well now
            // we're only writing on the main thread :D
            ZoneScopedN("Pre-transition");
            for (size_t i = 0; i < group_active.size(); ++i)
                ExecuteBarriers(passes[group_active[i]], &execute_barriers[i]);
        }
        {
            ZoneScopedN("Schedule Records");
            struct RecordJob : public Async::ThreadPoolJob
            {
                Renderer* r;
                ExecuteBarrierList* barriers;
                TrackedPass* pass;
                RHICommandList** cmd;
                size_t ord;
                // Write a lambda without writing a lambda
                // For demonstration of custom job types - and that
                // cmd buffers are thread-local - we don't get thread_id in lambda in my implementation
                RecordJob(Renderer* r, TrackedPass* pass, RHICommandList** cmd, size_t ord,
                          ExecuteBarrierList* barriers) : r(r), barriers(barriers), pass(pass), cmd(cmd), ord(ord)
                {
                }
                void Execute(size_t thread_id) noexcept override
                {
                    ZoneScoped;
                    ZoneNameF("<%s>", pass->name.c_str());
                    (*cmd) = r->ExecuteAllocateCommandList(pass->queue, thread_id);
                    (*cmd)->Begin();
                    (*cmd)->BeginTransition();
                    for (auto& [res, desc] : (*barriers))
                    {
                        res.Visit([&](RHIBuffer* p) { (*cmd)->SetBufferTransition(p, desc); },
                                  [&](RHITexture* p) { (*cmd)->SetImageTransition(p, desc); });
                    }
                    (*cmd)->EndTransition();
                    (*cmd)->DebugBegin(pass->name.c_str());
                    pass->pass->Record(pass->handle, r, *cmd);
                    (*cmd)->DebugEnd();
                    (*cmd)->End();
                };
            };
            for (size_t i = 0; i < group_active.size(); ++i)
                mExecuteThreadPool.PushImpl<RecordJob>(this, &passes[group_active[i]], &execute_cmds[i], i,
                                                       &execute_barriers[i]);
        }
        Vector<RHICommandList*> acq_cmds(mExecuteAlloc.Ptr()), rel_cmds(mExecuteAlloc.Ptr());
        bool needAcquire = false, needRelease = mSetup->executionGroups.size() > 1;
        if (group.groupIndex - 1 >= 0)
        {
            auto& prevGroup = mSetup->executionGroups[group.groupIndex - 1];
            needAcquire = prevGroup.queue != group.queue;
        }
        // Acquire resources for ourselves
        if (needAcquire)
        {
            auto cmd = ExecuteAllocateCommandList(group.queue, -1);
            cmd->Begin();
            cmd->DebugBegin("Group Acquire");
            ExecuteAcquireQueueResources(group.queue, group.groupIndex, cmd);
            cmd->DebugEnd();
            cmd->End();
            acq_cmds.emplace_back(cmd);
        }
        // Release resources to the next group if needed
        // since we can *only* do that on this queue
        if (needRelease)
        {
            auto cmd = ExecuteAllocateCommandList(group.queue, -1);
            cmd->Begin();
            cmd->DebugBegin("Group Release");
            ExecuteReleaseQueueResources(group.queue, group.groupIndex, cmd);
            cmd->DebugEnd();
            cmd->End();
            rel_cmds.emplace_back(cmd);
        }
        /* -- Submission -- */
        {
            ZoneScopedN("Group Submit");
            // We'd only signal the current group per submit
            auto Counter = [&](size_t ord) { return mFrameSwapped * mSetup->executionGroups.size() + ord + 1LL; };
            // Counter for a frame prior
            auto CounterFF = [&](size_t ord) { return (mFrameSwapped - 1LL) * mSetup->executionGroups.size() + ord + 1LL; };
            RHIDeviceQueue::TimelinePair timeline_signal;
            if (group.queue == RHIDeviceQueueType::Graphics)
                timeline_signal =
                    RHIDeviceQueue::TimelinePair(mGraphicsTimeline.Get(), Counter(group.graphicsGroupIndex));
            else if (group.queue == RHIDeviceQueueType::Compute)
                timeline_signal =
                    RHIDeviceQueue::TimelinePair(mComputeTimeline.Get(), Counter(group.computeGroupIndex));
            else [[unlikely]]
                throw std::runtime_error("Unhandled queue type");
            // Sync with previous groups on a different queue
            // otherwise submissions on the same queue are ordered only _by the barriers_
            // The command list ordering itself does _NOT_ guarantee it!!
            // To my idiotic past self:
            // https://www.lunarg.com/wp-content/uploads/2021/08/Vulkan-Synchronization-SIGGRAPH-2021.pdf
            Vector<RHIDeviceQueue::TimelinePair> timeline_waits(mExecuteAlloc.Ptr());
            Vector<RHIPipelineStage> timeline_wait_stages(mExecuteAlloc.Ptr());
            if (maxGraphicsSyncGroup >= 0 && group.queue == RHIDeviceQueueType::Compute)
                timeline_waits.emplace_back(mGraphicsTimeline.Get(), Counter(maxGraphicsSyncGroup)),
                    timeline_wait_stages.push_back(group.allStages);
            if (maxComputeSyncGroup >= 0 && group.queue == RHIDeviceQueueType::Graphics)
                timeline_waits.emplace_back(mComputeTimeline.Get(), Counter(maxComputeSyncGroup)),
                    timeline_wait_stages.push_back(group.allStages);
            // A special case for the first group of a queue
            // Always synchronize with the _last_ group of the _last_ frame
            if ((group.computeGroupIndex == 0 || group.graphicsGroupIndex == 0) && mFrameSwapped > 0)
            {
                auto& lastGroup = mSetup->executionGroups.back();
                if (lastGroup.queue == RHIDeviceQueueType::Graphics)
                    timeline_waits.emplace_back(mGraphicsTimeline.Get(), CounterFF(lastGroup.graphicsGroupIndex)),
                        timeline_wait_stages.push_back(group.allStages);
                else if (lastGroup.queue == RHIDeviceQueueType::Compute)
                    timeline_waits.emplace_back(mComputeTimeline.Get(), CounterFF(lastGroup.computeGroupIndex)),
                        timeline_wait_stages.push_back(group.allStages);
                else [[unlikely]]
                    throw std::runtime_error("Unhandled queue type");
            }
            RHIDeviceFence* fence_ptr = nullptr;
            // Fence the queues for every frame
            // Only one fence per queue is needed since submissions are in order
            if (group.isLastCompute)
                fence_ptr = mSwaps[mCurrentSync].computeFence.Get();
            else if (group.isLastGraphics)
                fence_ptr = mSwaps[mCurrentSync].graphicsFence.Get();
            const bool is_last = static_cast<size_t>(group.groupIndex) == mSetup->executionGroups.size() - 1;
            // Submit to our own queue
            RHIDeviceQueue* queue = nullptr;
            if (group.queue == RHIDeviceQueueType::Graphics)
                queue = mGraphicsQueue;
            else if (group.queue == RHIDeviceQueueType::Compute)
                queue = mComputeQueue;
            else [[unlikely]]
                throw std::runtime_error("Unhandled queue type");
            // Prepare the final command list submission
            Vector<RHICommandList*> group_cmds(mExecuteAlloc.Ptr());
            group_cmds.reserve(acq_cmds.size() + rel_cmds.size() + execute_cmds.size());
            // Wait for all recording to finish
            auto waitForRecord = [&]()
            {
                ZoneScopedN("Wait for Record");
                mExecuteThreadPool.Join();
                // Insert all cmd lists
                // [acq, execute, rel]
                group_cmds.insert(group_cmds.end(), acq_cmds.begin(), acq_cmds.end());
                group_cmds.insert(group_cmds.end(), execute_cmds.begin(), execute_cmds.end());
                group_cmds.insert(group_cmds.end(), rel_cmds.begin(), rel_cmds.end());
            };
            if (is_last)
            {
                ZoneScopedN("Final Submit");
                if (!mDesc.present)
                {
                    waitForRecord();
                    queue->Submit({.timelineWaits = timeline_waits,
                                   .timelineSignals = {{{timeline_signal}}},
                                   .waitsStages = timeline_wait_stages,
                                   .cmdLists = group_cmds,
                                   .fence = fence_ptr});
                }
                else
                {
                    // Last group to submit, and we need to present
                    CHECK_MSG(group.queue == RHIDeviceQueueType::Graphics,
                              "FIXME-ExecuteFrame: Last pass ended on a non-Graphics queue");
                    // Transition the Backbuffer to Present.
                    auto cmd = ExecuteAllocateCommandList(RHIDeviceQueueType::Graphics, -1);
                    cmd->Begin();
                    cmd->DebugBegin("Present");
                    cmd->BeginTransition();
                    ExecuteBarrierSubresource(kInvalidHandle, mSetup->trackedResources[mSwaps[GetSwap()].backbuffer],
                                              RHITextureSubresourceRange::Create(), {},
                                              RHIPipelineStageBits::BottomOfPipe, RHITextureLayout::Present, cmd);
                    cmd->EndTransition();
                    cmd->DebugEnd();
                    cmd->End();
                    waitForRecord();
                    timeline_wait_stages.push_back(group.allStages | RHIPipelineStageBits::BottomOfPipe);
                    group_cmds.push_back(cmd);
                    {
                        // Finally..
                        queue->Submit({.timelineWaits = timeline_waits,
                                       .timelineSignals = {{{timeline_signal}}},
                                       .waits = {{mSwaps[mCurrentSync].present.Get()}},
                                       .waitsStages = timeline_wait_stages,
                                       .signals = {{mSwaps[GetSwap()].render.Get()}},
                                       .cmdLists = group_cmds,
                                       .fence = mSwaps[mCurrentSync].graphicsFence.Get()});
                        queue->Present({.imageIndex = GetSwap(),
                                        .swapchain = mSwapchain.Get(),
                                        .waits = {{mSwaps[GetSwap()].render.Get()}}});
                    }
                }
            }
            else
            {
                ZoneScopedN("Submit");
                waitForRecord();
                queue->Submit({.timelineWaits = timeline_waits,
                               .timelineSignals = {{{timeline_signal}}},
                               .waitsStages = timeline_wait_stages,
                               .cmdLists = group_cmds,
                               .fence = fence_ptr});
            }
        }
    }
}
void Renderer::EndExecute()
{
    ZoneScoped;
    CHECK_MSG(mState == State::Execute, "Renderer bad state ({}). EndExecute() may only be called once per frame.",
              mState);
    mCurrentSync = (mCurrentSync + 1) % mFrameSwaps;
    mFrameSwapped++;
    mState = State::PostSetup;
    FrameMark;
}
void Renderer::CmdSetPipeline(PassHandle pass, RHICommandList* cmd) const
{
    CHECK(mState == State::Execute);
    auto& tpass = mSetup->trackedPasses[pass];
    CHECK_MSG(tpass.pso.IsValid(), "Current pass has no Pipeline state.");
    cmd->SetPipeline({.pipeline = tpass.pso.Get(),
                      .type = tpass.isComputePass ? RHIDevicePipelineType::Compute : RHIDevicePipelineType::Graphics});
    if (!tpass.pDescriptorSets.empty())
        cmd->BindDescriptorSet(tpass.isComputePass ? RHIDevicePipelineType::Compute : RHIDevicePipelineType::Graphics,
                               tpass.pso.Get(), tpass.pDescriptorSets, 0);
    for (auto const& [index, ptr, layout_ptr] : tpass.pExternalDescriptorSets)
        CmdBindDescriptorSet(pass, cmd, index, ptr);
}
void Renderer::CmdBindDescriptorSet(PassHandle pass, RHICommandList* cmd, uint32_t index,
                                    RHIDeviceDescriptorSet* descriptor_set) const
{
    CHECK(mState == State::Execute);
    auto& tpass = mSetup->trackedPasses[pass];
    CHECK_MSG(tpass.pso.IsValid(), "Current pass has no Pipeline state.");
    cmd->BindDescriptorSet(tpass.isComputePass ? RHIDevicePipelineType::Compute : RHIDevicePipelineType::Graphics,
                           tpass.pso.Get(), {{descriptor_set}}, index);
}
void Renderer::CmdBeginGraphics(PassHandle pass, RHICommandList* cmd, RHIExtent2D const& extent,
                                Optional<RHIClearColor> const& clear_rtv,
                                Optional<RHIClearDepthStencil> const& clear_dsv)
{
    CHECK(mState == State::Execute);
    auto& tpass = mSetup->trackedPasses[pass];
    Vector<RHICommandList::GraphicsDesc::Attachment> rtvs(mExecuteAlloc.Ptr());
    if (tpass.writeBackbuffer)
    {
        const RHIExtent2D backbuffer = GetSwapchainExtent();
        CHECK_MSG(extent.x <= backbuffer.x && extent.y <= backbuffer.y,
                  "Graphics extent too large for Swapchain Backbuffer {}", mCurrentSync);
        rtvs.push_back({.imageView = DerefCurrentBackbufferView(pass), .clearColor = clear_rtv});
    }
    else
    {
        rtvs.reserve(tpass.rtvs.size());
        for (const auto& rtv : tpass.rtvs | std::views::keys)
        {
            auto const& [rhdl, desc] = mSetup->trackedViews[rtv];
            auto const& tres = mSetup->trackedResources[rhdl];
            RHITexture* res = DerefResource(rhdl).Get<RHITexture*>();
            CHECK_MSG(res->mDesc.extent.x >= extent.x && res->mDesc.extent.y >= extent.y,
                      "Graphics extent too large for Render Target on {}", tres.name);
            rtvs.push_back({.imageView = DerefTextureView(rtv), .clearColor = clear_rtv});
        }
    }
    if (tpass.dsv != kInvalidHandle)
    {
        auto const& [depth_hdl, desc] = mSetup->trackedViews[tpass.dsv];
        auto const& tres = mSetup->trackedResources[depth_hdl];
        RHITexture* res = DerefResource(depth_hdl).Get<RHITexture*>();
        CHECK_MSG(res->mDesc.extent.x >= extent.x && res->mDesc.extent.y >= extent.y,
                  "Graphics extent too large for Depth buffer {}", tres.name);
        cmd->BeginGraphics({.colorAttachments = rtvs,
                            .depthAttachment = {.imageView = DerefTextureView(tpass.dsv),
                                                .imageLayout = RHITextureLayout::DepthStencil,
                                                .clearDepthStencil = clear_dsv},
                            .width = extent.x,
                            .height = extent.y});
    }
    else
    {
        CHECK_MSG(!rtvs.empty(), "No RTVs or DSV bound for graphics pass {} [{}]", tpass.name, pass)
        cmd->BeginGraphics({.colorAttachments = rtvs, .width = extent.x, .height = extent.y});
    }
}
RHIExtent3D Renderer::CmdGetComputeLocalSize(const PassHandle pass) const
{
    CHECK(mState == State::Execute);
    auto& tpass = mSetup->trackedPasses[pass];
    auto const& [x, y, z] = tpass.groupLocalSize;
    CHECK_MSG(x > 0 && y > 0 && z > 0, "Pass {} does not have a valid group local size", tpass.name);
    return {x, y, z};
}
void Renderer::CmdDispatch(const PassHandle pass, RHICommandList* cmd, const RHIExtent3D thread_size) const
{
    CHECK(mState == State::Execute);
    auto const& local_size = CmdGetComputeLocalSize(pass);
    cmd->Dispatch((thread_size.x + local_size.x - 1) / local_size.x, (thread_size.y + local_size.y - 1) / local_size.y,
                  (thread_size.z + local_size.z - 1) / local_size.z);
}
/* -- Debug -- */
String Renderer::DbgDumpGraphviz() const
{
    String out;
    fmt::format_to(std::back_inserter(out), "digraph G {{\n");
    fmt::format_to(std::back_inserter(out), "    rankdir=TB;\n");
    auto& graph = mSetup->graph;
    auto& passes = mSetup->trackedPasses;
    auto& resources = mSetup->trackedResources;
    for (auto& pass : passes)
    {
        fmt::format_to(std::back_inserter(out), "    \"{}@{}\" [ shape=box style={} fillcolor=\"{}\" ];\n", pass.name,
                       pass.handle, pass.used ? "filled" : "unfilled",
                       pass.queue == RHIDeviceQueueType::Graphics ? "#d0e0f0" : "#f0d0e0");
    }
    // Dependencies
    for (PassHandle u = 0; u < mSetup->graph.size(); u++)
    {
        for (auto [v, w] : graph[u])
        {
            auto const& resName = w != kInvalidHandle ? resources[w].name : "<reserved or Backbuffer>";
            fmt::format_to(std::back_inserter(out), "    \"{}@{}\" -> \"{}@{}\" [label=\"{}\"];\n", passes[u].name, u,
                           passes[v].name, v, resName);
        }
    }
    fmt::format_to(std::back_inserter(out), "}}\n");
    out.pop_back();
    return out;
}

String Renderer::DbgDumpActivePasses() const
{
    String out;
    for (const auto& idx : mSetup->execution)
    {
        auto& pass = mSetup->trackedPasses[idx];
        fmt::format_to(std::back_inserter(out),
                       "{}: {}, depth={}, pri={}, ord={}, queue={}, group={}, write_backbuffer={}\n", pass.handle,
                       pass.name, pass.depth, pass.priority, pass.ord, pass.queue, pass.groupIndex,
                       pass.writeBackbuffer);
    }
    out.pop_back();
    return out;
}

String Renderer::DbgDumpExecutionGroups() const
{
    String out;
    for (const auto& group : mSetup->executionGroups)
    {
        fmt::format_to(std::back_inserter(out), "{}: queue={}, stages={:b}, passes=[", group.groupIndex, group.queue,
                       static_cast<uint32_t>(group.allStages));
        for (const auto& pass : group.passes)
            fmt::format_to(std::back_inserter(out), "{} ", pass);
        out.pop_back();
        fmt::format_to(std::back_inserter(out), "]\n");
    }
    out.pop_back();
    return out;
}
