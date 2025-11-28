#include <filesystem>
#include <fstream>
#include <tracy/Tracy.hpp>

using namespace Foundation::Core;
using namespace Foundation::RenderCore;

Renderer::Renderer(RendererDesc const& desc, RHIApplicationHandle<RHIDevice> device,
                   RHIDeviceHandle<RHISwapchain> swapchain, Allocator* allocator) :
    mState(State::Undefined), mAllocator(allocator), mDesc(desc), mSwaps(mAllocator), mDevice(device),
    mSwapchain(swapchain), mExecuteArena(mAllocator, kExecuteArenaSize), mExecuteAlloc(mExecuteArena),
    mExecuteSubmits(nullptr), mExecuteThreadPool(mDesc.threadCount, kMaxCommandListsPerThread * 2, allocator, "Renderer"),
    mExecutePerSwapCmds(allocator), mWaitIdle(device.Get())
{
    mGraphicsQueue = mDevice->GetDeviceQueue(RHIDeviceQueueType::Graphics);
    mGraphicsQueue->DebugSetObjectName("Graphics Queue");
    mComputeQueue = mDevice->GetDeviceQueue(RHIDeviceQueueType::Compute);
    mComputeQueue->DebugSetObjectName("Compute Queue");
    LOG(Renderer, LogDebug, "** Renderer Init **");
    LOG(Renderer, LogDebug, "Async Compute:\t{}", mDesc.asyncCompute);
    LOG(Renderer, LogDebug, "Presentation:\t{}", mDesc.present);
    LOG(Renderer, LogDebug, "Threads:\t{}", mDesc.threadCount);
    LOG(Renderer, LogDebug, "PSO Cache:\t{:x}", reinterpret_cast<uintptr_t>(mDesc.pipelineCache));
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
void Renderer::BindPass(PassHandle pass, PassHandle other)
{
    CHECK(mState == State::Setup);
    mSetup->add_edge(pass, other, kInvalidHandle);
    mSetup->trackedPasses[pass].bindPasses.emplace_back(other);
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
void Renderer::BindShader(PassHandle pass, RHIShaderStage stage, StringView entry_point, const char* shader_path, Span<const char> specializationData) const
{
    CHECK(mState == State::Setup);
    CHECK_MSG(stage.is_bitmask(), "Only one stage can be bound to a shader per pass");
    for (auto const& [path, ep, st, spec] : mSetup->trackedPasses[pass].shaders)
        CHECK_MSG(!(st & stage),
                  "Shader stage {} already bound to {} in this pass. There can be at most one shader program per "
                  "shader stage per pass",
                  st, path);
    auto& [path, ep, st, spec] = mSetup->trackedPasses[pass].shaders.emplace_back(shader_path, entry_point, stage, mAllocator);
    spec.insert(spec.end(), specializationData.begin(), specializationData.end());
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
void Renderer::BindDescriptorSet(PassHandle pass, StringView bind_point, RHIDeviceDescriptorSetLayout* layout)
{
    CHECK(mState == State::Setup);
    mSetup->trackedPasses[pass].externalBindings.emplace_back(bind_point, layout, ~0u);
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
    tpass.backbufferRTV = blending;
    if (mSetup->lastBackbufferProducer != kInvalidHandle)
        mSetup->add_edge(pass, mSetup->lastBackbufferProducer, kInvalidHandle);
    mSetup->lastBackbufferProducer = pass;
}
void Renderer::BindBackbufferUAV(PassHandle pass, int set_index) const
{
    CHECK(mState == State::Setup);
    auto& tpass = mSetup->trackedPasses[pass];
    tpass.backbufferUAV = set_index;
    if (mSetup->lastBackbufferProducer != kInvalidHandle)
        mSetup->add_edge(pass, mSetup->lastBackbufferProducer, kInvalidHandle);
    mSetup->lastBackbufferProducer = pass;
    mSetup->bindingCounts[RHIDescriptorType::StorageImage]++;
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
        FinalizePasses();
    }
    else
    {
        LOG(Renderer, LogWarn, "No passes created in render graph.");
    }
    mState = State::PostSetup;
}
void Renderer::CullPasses(PassHandle epilogue) const
{
    CHECK(mState == State::Setup);
    CHECK(epilogue < mSetup->trackedPasses.size());
    // Cull and topsort
    PriorityQueue<Pair<int, PassHandle>> pq(mAllocator); // (pri, node)
    Vector<PassHandle> topo(mAllocator);
    Vector<int> dis(mSetup->trackedPasses.size(), 0, mAllocator);
    auto& in = mSetup->in;
    for (auto& pass : mSetup->trackedPasses)
    {
        // Always visit epilogue first
        if (pass.handle >= in.size() || in[pass.handle] == 0)
            pq.emplace(pass.handle == epilogue ? std::numeric_limits<int>::max() : pass.priority, pass.handle);
    }
    // BFS with priority <pri then insertion order (handle value)>
    while (!pq.empty())
    {
        auto [pri, u] = pq.top();
        pq.pop();
        topo.push_back(u);
        if (mSetup->graph.size() > u)
        {
            for (const auto& v : mSetup->graph[u] | std::views::keys)
            {
                in[v]--;
                dis[v] = std::max(dis[v], dis[u] + 1);
                if (in[v] == 0)
                    pq.emplace(mSetup->trackedPasses[v].priority, v);
            }
        }
    }
    // Sort by longest path
    // Ordering is still valid topological order
    Ranges::sort(topo, [&](auto const& a, auto const& b) { return dis[a] > dis[b]; });
    auto& exec = mSetup->execution;
    exec = topo;
    mSetup->epilogue = epilogue;
    // Collect active resources
    for (PassHandle ord = 0; ord < exec.size(); ord++)
    {
        auto& pass = mSetup->trackedPasses[exec[ord]];
        pass.used = true;
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
    auto& groups = mSetup->executionGroups;
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
        auto& group =
            groups.emplace_back(static_cast<int>(groups.size()), mSetup->trackedPasses[exec[i]].queue, mAllocator);
        group.passes.insert(group.passes.end(), exec.begin() + i, exec.begin() + j);
        // Collect dependencies
        for (auto pass : group.passes)
        {
            auto& tpass = mSetup->trackedPasses[pass];
            tpass.groupIndex = group.groupIndex;
            group.resources.insert(group.resources.end(), tpass.resources.begin(), tpass.resources.end());
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
        auto it = Ranges::find_if(groups | Views::reverse,
                                  [](auto const& g) { return g.queue == RHIDeviceQueueType::Graphics; });
        if (it != groups.rend())
            it->isLastGraphics = true;
        it = Ranges::find_if(groups | Views::reverse,
                             [](auto const& g) { return g.queue == RHIDeviceQueueType::Compute; });
        if (it != groups.rend())
            it->isLastCompute = true;
    }
    // Assign graphics/compute group indices
    {
        for (auto& g : groups)
        {
            if (g.queue == RHIDeviceQueueType::Graphics)
                g.graphicsGroupIndex = mSetup->executionNumGraphicsGroups++;
            else if (g.queue == RHIDeviceQueueType::Compute)
                g.computeGroupIndex = mSetup->executionNumComputeGroups++;
        }
    }
    LOG(Renderer, LogDebug, "** Render Graph GraphViz **\n{}", DbgDumpGraphviz());
    LOG(Renderer, LogDebug, "** Render Graph Execution Order **\n{}", DbgDumpActivePasses());
    LOG(Renderer, LogDebug, "** Render Graph Execution Groups **\n{}", DbgDumpExecutionGroups());
}
// NOTE: Assumes pass is fresh or already reset.
void Renderer::BuildPipelineState(PassHandle pass)
{
    CHECK_MSG(mState == State::Setup || mState == State::PostSetup, "Invalid state {}", mState);
    auto& tracked = mSetup->trackedPasses[pass];
#pragma region Shader Bytecode
    /* -- Parse shader Bytecode -- */
    Vector<RHIPipelineState::PipelineStateDesc::ShaderStage> pso_stages(mAllocator);
    if (tracked.shaders.empty())
        return; // Pass with no shaders
    LOG(Renderer, LogInfo, "** Building PSO for {} [{}] **", tracked.name, pass);
    Vector<char> data(mAllocator);
    Map<String, RHIDeviceScopedHandle<RHIShaderModule>> shaders(mAllocator);
    Map<String, UniquePtr<Shader>> reflections(mAllocator);
    Map<String, Span<const char>> specializations(mAllocator);
    for (auto const& [shader_path, entry_point, stage, spec] : tracked.shaders)
    {
        if (!shaders.contains(shader_path))
        {
            std::error_code ec;
            auto size = std::filesystem::file_size(shader_path, ec);
            CHECK_MSG(!ec, "Failed to open shader file {}: {}", shader_path, ec.message());
            data.resize(size);
            std::ifstream file(shader_path, std::ios::binary);
            CHECK_MSG(file.is_open() && file.read(data.data(), size), "Failed to read shader {}", shader_path);
            reflections.emplace(shader_path, ConstructUnique<Shader>(mAllocator, data, mAllocator));
            shaders[shader_path] = mDevice->CreateShaderModule({.source = data});
            shaders[shader_path]->DebugSetObjectName(shader_path.c_str());
            specializations[shader_path] = spec;
        }
        auto& module = shaders[shader_path];
        // In BindShader we have already guaranteed these to be unique per stage
        if (stage & RHIShaderStageBits::Compute)
            tracked.isComputePass = true, tracked.piplineStages |= RHIPipelineStageBits::ComputeShader;
        if (stage & RHIShaderStageBits::Fragment)
            tracked.piplineStages |= RHIPipelineStageBits::FragmentShader;
        if (stage & RHIShaderStageBits::Vertex)
            tracked.piplineStages |= RHIPipelineStageBits::VertexShader;
        if (stage & RHIShaderStageBits::Mesh)
            tracked.piplineStages |= RHIPipelineStageBits::MeshShader;
        if (stage & RHIShaderStageBits::Task)
            tracked.piplineStages |= RHIPipelineStageBits::TaskShader;
        bool found = false;
        for (auto const& ep : reflections[shader_path]->mEntrypoints)
        {
            if (ep.stage == stage && ep.name == entry_point)
            {
                // Check specialization constant status
                // Our RHI only allows zero or one decl, hence spec is only a binary blob for one element
                auto& specDecl = reflections[shader_path]->mSpecializationConstants;
                CHECK_MSG((!spec.empty() && specDecl.size() == 1) || (spec.empty() && specDecl.empty()), "Only zero or one Specialization Constants is allowed in shader bytecode. Got {}", specDecl.size());
                if (!specDecl.empty())
                    CHECK_MSG(specDecl.front().id == 0, "Expected Specialization Constant ID to be 0, got {}.", specDecl.front().id);
                pso_stages.push_back(
                    {.desc = {
                        .stage = stage,
                        .entryPoint = ep.name.c_str(),
                        .specializationData = specializations[shader_path],
                    }, .shaderModule = module.Get()});
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
    }
#pragma endregion
#pragma region Descriptor Sets
    /* -- Descriptor Set Build -- */
    // Check variable bindings to be consistent across stages
    // [name, [set, binding]]
    Map<String, Pair<uint32_t, uint32_t>> refl_var_bind_points(mAllocator);
    Optional<int> backbufferUAVUsage; // opt: set index
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
            {
                if (tracked.backbufferUAV == bind.descriptorSet)
                {
                    CHECK_MSG(bind.binding == 0 && !backbufferUAVUsage.has_value(),
                              "Set {} is used for Backbuffer UAV, which can only contain one binding at 0 (got {})",
                              bind.descriptorSet, bind.binding);
                    backbufferUAVUsage = bind.descriptorSet;
                    continue;
                }
                refl_var_bind_points[bind.name] = {bind.descriptorSet, bind.binding};
            }
            else
            {
                auto& [set, binding] = it->second;
                CHECK_MSG(set == bind.descriptorSet && binding == bind.binding,
                          "Inconsistent binding points across shader stages for variable {} in shader {}", bind.name,
                          path);
            }
        }
    }
    // Check that all [set,binding] are unique, and bindings per set is contiguous
    Map<Pair<int,int>, String> bind_unique(mAllocator); // set, binding, name
    for (auto& [name, bind] : refl_var_bind_points)
    {
        auto it = bind_unique.find(bind);
        CHECK_MSG(it == bind_unique.end(), "Binding set {} binding {} is used by both {} and {} in pass {}.",
                  bind.first, bind.second, name, it->second, tracked.name);
        bind_unique[bind] = name;
    }
    // Check per-set binding contiguity
    for (uint32_t set = 0; ; set++)
    {
        Pair<int, int> key {set, 0};
        auto it = bind_unique.find(key);
        if (it == bind_unique.end())
            break;
        int cbindings = 0, last_binding = 0;
        for (; it != bind_unique.end() && it->first.first == set; it++)
        {        
            last_binding = it->first.second;
            cbindings++;
        }
        CHECK_MSG(last_binding == cbindings - 1,
                  "Pass {} Binding set {} has non-contiguous bindings (max binding {}, count {}). Last binding {}.", tracked.name, set,
                  last_binding, cbindings, bind_unique[{set, last_binding}]);
    }
    // Check descriptor set layout to be consistent across stages
    Map<String, RHIDescriptorType> var_types(mAllocator);
    Map<String, ResourceHandle> var_handles(mAllocator);
    Map<String, ResourceHandle> var_samplers(mAllocator);
    Map<String, RHIDeviceDescriptorSetLayout*> var_ext_sets(mAllocator);
    // ...for Textures
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
    // ...for Buffers
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
    // ...for Samplers
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
    // External sets (e.g. @ref BindlessPool)
    std::sort(tracked.externalBindings.begin(), tracked.externalBindings.end());
    for (auto& [binding, desc_set_layout, set] : tracked.externalBindings)
    {
        // We don't create anything for these sets, user provides the layouts - but we do resolve these
        // to actual [set,binding] pairs so they can be bound by bind points (var names)
        if (refl_var_bind_points.contains(binding))
        {
            auto bind = refl_var_bind_points[binding];
            CHECK_MSG(bind.second < desc_set_layout->mDesc.bindings.size(),
                      "Shader binding {} at set {} exceeded declared layout binding {} size {} in pass {}.", binding,
                      binding, bind.first, desc_set_layout->mDesc.bindings.size(), pass);
            set = bind.first, tracked.pExternalDescriptorSets.emplace_back(set, desc_set_layout);
        }
        else
        {
            CHECK_MSG(false, "External descriptor set binding {} is not used by any shader in pass {}", binding,
                      tracked.name);
        }
        var_ext_sets[binding] = desc_set_layout;
    }
    // Sort by set index
    std::sort(tracked.pExternalDescriptorSets.begin(), tracked.pExternalDescriptorSets.end());
    tracked.pExternalDescriptorSets.erase(
        std::unique(tracked.pExternalDescriptorSets.begin(), tracked.pExternalDescriptorSets.end()),
        tracked.pExternalDescriptorSets.end());
    // [[set, binding], name]
    Vector<Pair<Pair<uint32_t, uint32_t>, String>> bindings(mAllocator);
    bindings.reserve(var_types.size());
    for (auto& [name, bind] : refl_var_bind_points)
    {
        if (!var_ext_sets.contains(name))
            bindings.emplace_back(bind, name);
    }
    std::sort(bindings.begin(), bindings.end());
    // Separate into descriptor sets
    Vector<RHIDeviceDescriptorSetLayoutDesc::Binding> set_bindings(mAllocator);
    for (const auto& binding : bindings | Views::values)
    {
        CHECK_MSG(var_types.contains(binding) || var_ext_sets.contains(binding),
                  "Binding {} is not bound by pass {}, but is used by one of its shaders.", binding, tracked.name);
        set_bindings.push_back({.count = 1, .stage = RHIShaderStageBits::All, .type = var_types[binding]});
    }
    // Check if our first set is not 0
    if (!bindings.empty() && bindings[0].first.first != 0)
    {
        LOG(BuildPipelineState, LogError,
            "Binding set numbers must start from 0. Error at set {} binding {} in pass {}.", bindings[0].first.first,
            bindings[0].first.second, tracked.name);
        CHECK_MSG(false, "Binding set numbers must start from 0.");
    }
    for (uint32_t i = 0, j = 0; i < bindings.size(); i = j)
    {
        uint32_t set = bindings[i].first.first;
        // Check if our first binding is not 0
        if (bindings[i].first.second != 0)
        {
            LOG(BuildPipelineState, LogError,
                "Binding numbers must start from 0 in each descriptor set. Error at set {} binding {} in pass {}.", set,
                bindings[i].first.second, tracked.name);
            CHECK_MSG(false, "Binding binding must start from 0.");
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
                    ds->Update({.binding = binding, .type = type, .images = {{{.sampler = sampler}}}});
                    break;
                }
            case SampledImage:
            case StorageImage:
                {
                    auto* view = DerefTextureView(hdl);
                    ds->Update({.binding = binding,
                                .type = type,
                                .images = {{{.imageView = view,
                                             .layout = type == SampledImage ? RHITextureLayout::ShaderReadOnly
                                                                            : RHITextureLayout::General}}}});
                    break;
                }
            case UniformBuffer:
            case StorageBuffer:
                {
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
    // We've already established that these would not conflict with our own sets, and has already been sorted
    for (auto const& [ptr, layout_ptr] : tracked.pExternalDescriptorSets)
    {
        tracked.pDescriptorLayouts.emplace_back(layout_ptr);
    }
    // Add Backbuffer UAV set if needed
    if (backbufferUAVUsage.has_value())
    {
        auto set = backbufferUAVUsage.value();
        tracked.pDescriptorLayouts.resize(std::max(tracked.pDescriptorLayouts.size(), static_cast<size_t>(set + 1u)));
        tracked.pDescriptorLayouts[set] = mSwapDescriptorSetLayout.Get();
    }
#pragma endregion
#pragma region PSO
    /* -- Pipeline State Creation -- */
    if (!pso_stages.empty())
    {
        tracked.pso.Reset();
        RHIPipelineState::PipelineStateDesc pso_desc{
            .psoCache = mDesc.pipelineCache,
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
        if (tracked.backbufferRTV)
        {
            attachments.push_back(
                {.blending = tracked.backbufferRTV.value(), .renderTarget = {.format = mSwapchain->mDesc.format}});
        }
        for (auto const& [rtv, blending] : tracked.rtvs)
        {
            auto& [rhdl, desc] = mSetup->trackedViews[rtv];
            attachments.push_back({.blending = blending, .renderTarget = {.format = desc.format}});
        }
        pso_desc.attachments = attachments;
        pso_desc.depthStencil.depthTest = pso_desc.depthStencil.depthWrite = tracked.dsv != kInvalidHandle;
        if (pso_desc.depthStencil.depthTest)
        {
            auto& [dsv_handle, desc] = mSetup->trackedViews[tracked.dsv];
            pso_desc.depthStencil.depthFormat = desc.format;
        }
        tracked.pso = mDevice->CreatePipelineState(pso_desc);
        tracked.pso->DebugSetObjectName(fmt::format("PSO of {} [{}]", tracked.name, pass).c_str());
    }
    else
    {
        LOG(Renderer, LogDebug, "Pass {} has no shader stages, and thus no PSO is created.", tracked.name);
    }
#pragma endregion
}
void Renderer::BuildPipelineStateAll()
{
    CHECK(mState == State::Setup | mState == State::PostSetup);
    LOG(Renderer, LogInfo, "Compiling Shaders");
    ThreadPool pool(std::thread::hardware_concurrency(), kMaxRenderPasses, mAllocator, "PSOComp");
    Vector<Pair<PassHandle,Future<void>>> futures(mAllocator);
    for (auto& pass : mSetup->trackedPasses)
    {
        if (!pass.used)
            continue;
        auto handle = pass.handle;
        futures.emplace_back(handle, pool.Push([this, handle] { BuildPipelineState(handle); }));
    }
    for (auto& [pass, future] : futures)
    {
        auto& tpass = mSetup->trackedPasses[pass];
        try
        {
            future.get();
        }
        catch (std::runtime_error const& e)
        {
            LOG(Renderer, LogError, "Failed to build PSO for pass {}: {}", tpass.name, e.what());
            throw; // Failfast
        }
    }
    LOG(Renderer, LogInfo, "Compiled Shaders.");
}
void Renderer::FinalizePasses()
{
    CHECK(mState == State::Setup | mState == State::PostSetup);
    // Build descriptor pool
    mDescPool.Reset();
    if (!mSetup->bindingCounts.empty())
    {
        Vector<RHIDeviceDescriptorPool::PoolDesc::Binding> bindings(mAllocator);
        bindings.reserve(mSetup->bindingCounts.size());
        LOG(Renderer, LogDebug, "** Descriptor Pool **");
        for (auto& [type, count] : mSetup->bindingCounts)
        {
            LOG(Renderer, LogDebug, "\t{}: {}", type, count);
            bindings.push_back({.type = type, .maxCount = count});
        }
        mDescPool = mDevice->CreateDescriptorPool({bindings});
        mDescPool->DebugSetObjectName("Renderer Descriptor Pool");
    }
    BuildPipelineStateAll();
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
        bool needShared = res.hasComputeUsage && res.hasGraphicsUsage;
        res.desc.Visit(
            // Owned
            [&](RHIBufferDesc const& desc)
            {
                RHIBufferDesc maybeShared = desc;
                if (needShared)
                    maybeShared.resource.shared = true,
                    maybeShared.resource.sharedQueues =
                        RHIDeviceQueueFlagsBits::Graphics | RHIDeviceQueueFlagsBits::Compute;
                mResources->resources[handle] = mDevice->CreateBuffer(maybeShared);
                DerefResource(handle).Get<RHIBuffer*>()->DebugSetObjectName(
                    fmt::format("{} [{}]", res.name, handle).c_str());
            },
            [&](RHITextureDesc const& desc)
            {
                RHITextureDesc maybeShared = desc;
                if (needShared)
                    maybeShared.resource.shared = true,
                    maybeShared.resource.sharedQueues =
                        RHIDeviceQueueFlagsBits::Graphics | RHIDeviceQueueFlagsBits::Compute;
                mResources->resources[handle] = mDevice->CreateTexture(maybeShared);
                DerefResource(handle).Get<RHITexture*>()->DebugSetObjectName(
                    fmt::format("{} [{}]", res.name, handle).c_str());
            },
            // Borrowed
            [&](RHIDeviceHandle<RHIBuffer> const& hdl) { mResources->resources[handle] = hdl; },
            [&](RHIDeviceHandle<RHITexture> const& hdl) { mResources->resources[handle] = hdl; },
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
            mResources->fit(handle);
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
        while (threads.size() < mDesc.threadCount + 1) // Inc. main (render) thread. We do work too!
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
    mGraphicsTimeline->DebugSetObjectName(fmt::format("Graphics Timeline Semaphore").c_str());
    mComputeTimeline = mDevice->CreateSemaphore(true);
    mComputeTimeline->DebugSetObjectName(fmt::format("Async Compute Timeline Semaphore").c_str());
}
void Renderer::SetSwapchain(RHIDeviceHandle<RHISwapchain> swapchain)
{
    CHECK_MSG(mDesc.present, "Cannot set swapchain when the renderer is not created with Present support");
    CHECK_MSG(swapchain.IsValid(), "Cannot set swapchain when swapchain is not valid");
    mFrameSwaps = swapchain->GetImages().size();
    LOG(Renderer, LogInfo, "Swapchain uses {} back buffers", mFrameSwaps);
    if (mState == State::Execute)
    {
        // If changing swapchain during execution (e.g. due to resize exception)
        // Wait for GPU to be idle
        LOG(Renderer, LogInfo, "Swapchain is already in execute??");
        mDevice->WaitIdle();
        mState = State::PostSetup;
    }
    SetFrameSyncObjects();
    for (size_t i = 0; i < mFrameSwaps; ++i)
    {
        auto* backbuffer = swapchain->GetImages()[i];
        backbuffer->DebugSetObjectName(fmt::format("Backbuffer {}", i).c_str());
        mSwaps[i].view = backbuffer->CreateTextureView(
            RHITextureViewDesc{.format = swapchain->mDesc.format, .range = RHITextureSubresourceRange::Create()});
        // Create one descriptor set per backbuffer
        // Ideally - we should use Push Descriptors for these - alas we're stuck with Vulkan 1.3 for now.
        // @ref CmdSetPipeline would set these up automatically if a pass wants backbuffer UAV/SRV.
        mSwapDescriptorSetLayout =
            mDevice->CreateDescriptorSetLayout({.bindings = {{{.type = RHIDescriptorType::StorageImage}}}});
        mSwapDescriptorPool = mDevice->CreateDescriptorPool(
            {.bindings = {{{.type = RHIDescriptorType::StorageImage, .maxCount = mFrameSwaps}}}});
        mSwaps[i].viewSet = mSwapDescriptorPool->CreateDescriptorSet(mSwapDescriptorSetLayout);
        mSwaps[i].viewSet->Update(
            {.type = RHIDescriptorType::StorageImage,
             .images = {{{.imageView = mSwaps[i].view.Get(), .layout = RHITextureLayout::General}}}});
        if (mSwaps[i].backbuffer == kInvalidHandle)
        {
            // First time setup
            mSwaps[i].backbuffer = CreateResource(fmt::format("Backbuffer {}", i), backbuffer);
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
    mExecuteSubmits = Construct<Vector<Pair<RHIDeviceQueueType, RHIDeviceQueue::SubmitDesc>>>(mExecuteAlloc.Ptr(),
                                                                                              mExecuteAlloc.Ptr());
    Vector<RHIDeviceFence*> wait(mExecuteAlloc.Ptr());
    if (mSetup->executionAnyGraphics)
        wait.push_back(mSwaps[mCurrentSync].graphicsFence.Get());
    if (mSetup->executionAnyCompute)
        wait.push_back(mSwaps[mCurrentSync].computeFence.Get());
    if (wait.size())
    {
        ZoneScopedN("Wait for GPU");
        mDevice->WaitForFences(wait, true, -1);
        mDevice->ResetFences(wait);
    }
    if (mDesc.present && mSetup->executionAnyGraphics)
    {
        ZoneScopedN("Acquire Next Image");
        mCurrentSwap = mSwapchain->GetNextImage(-1, mSwaps[mCurrentSync].present, {});
    }
    {
        ZoneScopedN("Reset Cmds");
        // Reset per-swap command lists
        for (auto& cmds : mExecutePerSwapCmds[mCurrentSync])
            cmds->Reset();
    }
}
void Renderer::ExecuteBarrierSubresourceState(PassHandle pass, RHITexture* res, TrackedResource::SubresourceState& sta,
                                              RHIResourceAccess access, RHIPipelineStage stage, RHITextureLayout layout,
                                              ExecuteBarrierPCmdOrPBarrierList cmd) const
{
    ZoneScoped;
    // RW resources need barriers even if the state doesn't change
    // because of potential WOW hazards
    if ((sta.access & kAllShaderWrites) != 0 || (access & kAllShaderWrites) != 0)
    {
        /* always barrier */
    }
    else if (mFrameSwapped != 0 && sta.access == access && sta.stage == stage && sta.layout == layout)
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
    if (access & kAllShaderWrites)
    {
        sta.lastProducer = pass;
        sta.lastProducedFrame = mFrameSwapped;
    }
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
    /* Same as textures */
    if ((tres.lastBufferState.access & kAllShaderWrites) != 0 || (access & kAllShaderWrites) != 0)
    {
        /* always barrier */
    }
    else if (mFrameSwapped != 0 && tres.lastBufferState.access == access && tres.lastBufferState.stage == stage)
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
    if (access & kAllShaderWrites)
    {
        tres.lastBufferState.lastProducer = pass;
        tres.lastBufferState.lastProducedFrame = mFrameSwapped;
    }
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
    if (pass.backbufferRTV)
    {
        CHECK_MSG(pass.queue == RHIDeviceQueueType::Graphics, "Backbuffer RTV can only be used in Graphics queue");
        const RHIResourceAccess rt_access =
            RHIResourceAccessBits::RenderTargetWrite | RHIResourceAccessBits::RenderTargetRead;
        const RHITextureLayout rt_layout = RHITextureLayout::RenderTarget;
        const RHIPipelineStage rt_stage = RHIPipelineStageBits::RenderTargetOutput;
        auto& tres = mSetup->trackedResources[mSwaps[GetSwap()].backbuffer];
        ExecuteBarrierSubresource(pass.handle, tres, RHITextureSubresourceRange::Create(), rt_access, rt_stage,
                                  rt_layout, cmd);
    }
    if (pass.backbufferUAV)
    {
        const RHIResourceAccess rt_access = RHIResourceAccessBits::ShaderWrite;
        const RHITextureLayout rt_layout = RHITextureLayout::General;
        const RHIPipelineStage rt_stage = RHIPipelineStageBits::ComputeShader;
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
Renderer::ExecutePerThreadCommandLists::ExecutePerThreadCommandLists(RHIDevice* device, const size_t maxPerThread,
                                                                     Allocator* alloc) :
    graphicsCmds(maxPerThread, alloc), computeCmds(maxPerThread, alloc)
{
    graphicsPool = device->CreateCommandPool(
        {.queue = device->GetDeviceQueue(RHIDeviceQueueType::Graphics), .type = RHICommandPoolType::Transient});
    computePool = device->CreateCommandPool(
        {.queue = device->GetDeviceQueue(RHIDeviceQueueType::Compute), .type = RHICommandPoolType::Transient});
}
void Renderer::ExecutePerThreadCommandLists::Reset()
{
    graphicsCtr = 0;
    computeCtr = 0;
    graphicsPool->ResetAllCommandLists(false /* freeResources */);
    computePool->ResetAllCommandLists(false /* freeResources */);
}
RHICommandList* Renderer::ExecutePerThreadCommandLists::AllocateGraphics()
{
    size_t index = graphicsCtr++;
    if (!graphicsCmds[index].IsValid())
    {
        graphicsCmds[index] = graphicsPool->CreateCommandList();
        graphicsCmds[index]->DebugSetObjectName(fmt::format("Graphics List {}", index).c_str());
    }
    return graphicsCmds[index].Get();
}
RHICommandList* Renderer::ExecutePerThreadCommandLists::AllocateCompute()
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
    CHECK_MSG(mState == State::Execute, "Renderer bad state ({}). Did you call BeginExecute()?", mState);
    auto& swap = mExecutePerSwapCmds[mCurrentSync];
    auto& thread = swap[thread_id + 1]; // thread_id == -1 is the main thread
    switch (queue)
    {
    case RHIDeviceQueueType::Compute:
        return thread->AllocateCompute();
    case RHIDeviceQueueType::Graphics:
        return thread->AllocateGraphics();
    default:
        throw std::runtime_error("Unsupported queue type for command list allocation");
    }
}
void Renderer::ExecuteFrame()
{
    ZoneScoped;
    CHECK_MSG(mState == State::Execute, "Renderer bad state ({}). Did you call BeginExecute()?", mState);
    auto& passes = mSetup->trackedPasses;
    // Execute by groups
    auto& groups = mSetup->executionGroups;
    for (auto& group : groups)
    {
        /* -- Inter queue sync -- */
        auto Counter = [&](size_t ord) { return mFrameSwapped * groups.size() + ord + 1LL; };
        auto CounterPrior = [&](size_t ord) { return (mFrameSwapped - 1LL) * groups.size() + ord + 1LL; };
        // Collect producers that branched out before this group
        // We only take groups that's produced in this frame before the current one
        int graphicsWaitValue = CounterPrior(mSetup->executionNumGraphicsGroups - 1);
        int computeWaitValue = CounterPrior(mSetup->executionNumComputeGroups - 1);
        // ...since by default - we use the last frame's values
        auto UpdateSyncGroup = [&](PassHandle pass, size_t frame)
        {
            if (pass == kInvalidHandle)
                return;
            auto& tpass = mSetup->trackedPasses[pass];
            auto& tgroup = groups[tpass.groupIndex];
            int graphicsValue =
                frame == mFrameSwapped ? Counter(tgroup.graphicsGroupIndex) : CounterPrior(tgroup.graphicsGroupIndex);
            int computeValue =
                frame == mFrameSwapped ? Counter(tgroup.computeGroupIndex) : CounterPrior(tgroup.computeGroupIndex);
            switch (tpass.queue)
            {
            case RHIDeviceQueueType::Graphics:
                graphicsWaitValue = std::max(graphicsValue, graphicsValue);
                break;
            default:
            case RHIDeviceQueueType::Compute:
                computeWaitValue = std::max(computeValue, computeValue);
                break;
            }
        };
        for (auto pass_handle : group.passes)
        {
            auto const& pass = mSetup->trackedPasses[pass_handle];
            // Explicit producers
            for (auto hdl : pass.bindPasses)
            {
                auto& tpass = mSetup->trackedPasses[hdl];
                UpdateSyncGroup(hdl, tpass.frameExec);
            }
            // Textures
            for (auto [hdl, access, stage, range, layout] : pass.textureUsages)
            {
                auto& tres = mSetup->trackedResources[hdl];
                for (auto const& sta : tres.GetLastSubresourceStateOf(range))
                    UpdateSyncGroup(sta.lastProducer, sta.lastProducedFrame);
            }
            // Buffers
            for (auto [hdl, access, stage] : pass.bufferUsages)
            {
                auto& tres = mSetup->trackedResources[hdl];
                UpdateSyncGroup(tres.lastBufferState.lastProducer, tres.lastBufferState.lastProducedFrame);
            }
            // Backbuffer
            if (pass.backbufferRTV || pass.backbufferUAV)
            {
                auto& tres = mSetup->trackedResources[mSwaps[GetSwap()].backbuffer];
                for (auto const& sta : tres.GetLastSubresourceStateOf(RHITextureSubresourceRange::Create()))
                    UpdateSyncGroup(sta.lastProducer, sta.lastProducedFrame);
            }
        }
        /* -- Recording -- */
        // Count only the non-skipped ones
        Vector<PassHandle> active(mExecuteAlloc.Ptr());
        active.reserve(groups.size() + 1);
        for (auto handle : group.passes)
        {
            if (!passes[handle].pass->IsSkipped(handle, this))
                active.emplace_back(handle);
        }
        // Record all the active tasks
        // If this is the last group and present is needed
        const bool needPresent = static_cast<size_t>(group.groupIndex) == groups.size() - 1 && mDesc.present;
        // Graphics then Compute - some resources (e.g. depth) needs to be transitioned on
        // and only on the most capable queue.
        size_t groupIndex = group.groupIndex;
        size_t nextGroupIndex = (groupIndex + 1) %
            groups.size(); // Next group. Would be the first group if current groupIndex is the last group
        bool needPostTransition = groups[groupIndex].queue == RHIDeviceQueueType::Graphics &&
            groups[nextGroupIndex].queue != RHIDeviceQueueType::Graphics;
        // Next - we do all the passes in parallel - with transitions starting before the passes
        auto passBarriers = ConstructSpan<ExecuteBarrierList>(mExecuteAlloc.Ptr(), active.size(), mExecuteAlloc.Ptr());
        auto passCmds =
            ConstructSpan<RHICommandList*>(mExecuteAlloc.Ptr(), active.size() + needPostTransition + needPresent);
        {
            // ExecuteBarriers - Set...Barrier calls are very, very cheap and doesn't reach the driver
            // until a call to EndTransition on the cmd
            ZoneScopedN("Pre-transition");
            for (size_t i = 0; i < active.size(); ++i)
                ExecuteBarriers(passes[active[i]], &passBarriers[i]);
        }
        {
            ZoneScopedN("Schedule Records");
            struct RecordJob : public ThreadPoolJob
            {
                Renderer* r;
                ExecuteBarrierList* barriers;
                TrackedPass* pass;
                RHICommandList** cmd;
                size_t ord;
                // Write a lambda without writing a lambda
                // For demonstration of custom job types - and that
                // cmd buffers are thread-local, plus how we don't get thread_id in lambda in my implementation
                RecordJob(Renderer* r, TrackedPass* pass, RHICommandList** cmd, size_t ord,
                          ExecuteBarrierList* barriers) : r(r), barriers(barriers), pass(pass), cmd(cmd), ord(ord)
                {
                }
                void Execute(size_t thread_id) noexcept override
                {
                    ZoneScoped;
                    ZoneNameF("<%s>", pass->name.c_str());
                    *cmd = r->ExecuteAllocateCommandList(pass->queue, thread_id);
                    (*cmd)->Begin();
                    (*cmd)->BeginTransition();
                    for (auto& [res, desc] : (*barriers))
                    {
                        res.Visit([&](RHIBuffer* p) { (*cmd)->SetBufferTransition(p, desc); },
                                  [&](RHITexture* p) { (*cmd)->SetImageTransition(p, desc); });
                    }
                    (*cmd)->EndTransition();
                    (*cmd)->DebugBegin(pass->name.c_str());
                    pass->frameExec = r->mFrameSwapped;
                    pass->pass->Record(pass->handle, r, *cmd);
                    (*cmd)->DebugEnd();
                    (*cmd)->End();
                }
            };
            for (size_t i = 0; i < active.size(); ++i)
            {
                if (mDesc.threadCount)
                {
                    mExecuteThreadPool.PushImpl<RecordJob>(this, &passes[active[i]], &passCmds[i], i, &passBarriers[i]);
                }
                else
                {
                    RecordJob job(this, &passes[active[i]], &passCmds[i], i, &passBarriers[i]);
                    job.Execute(-1); // Main thread
                }
            }
        }
        {
            ZoneScopedN("Post Transition / Present");
            // Only Compute resources _need_ to be transitioned here beforehand. So we only deal with that
            if (needPostTransition)
            {
                // Declare that the first pass from the next group handled the transition
                PassHandle executorPass = groups[nextGroupIndex].passes.front();
                auto cmd = ExecuteAllocateCommandList(RHIDeviceQueueType::Graphics, -1);
                cmd->Begin();
                cmd->DebugBegin("Graphics Pre Compute");
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
                }
                for (PassHandle pass : groups[nextGroupIndex].passes)
                {
                    auto& tracked = mSetup->trackedPasses[pass];
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
                cmd->DebugEnd();
                cmd->End();
                passCmds[active.size()] = cmd;
            }
            // Transition the backbuffer
            if (needPresent)
            {
                // Transition the Backbuffer to Present.
                auto cmd = ExecuteAllocateCommandList(RHIDeviceQueueType::Graphics, -1);
                cmd->Begin();
                cmd->BeginTransition();
                ExecuteBarrierSubresource(kInvalidHandle, mSetup->trackedResources[mSwaps[GetSwap()].backbuffer],
                                          RHITextureSubresourceRange::Create(), {}, RHIPipelineStageBits::BottomOfPipe,
                                          RHITextureLayout::Present, cmd);
                cmd->EndTransition();
                cmd->End();
                passCmds[active.size() + needPostTransition] = cmd;
            }
        }
        /* -- Submission -- */
        {
            ZoneScopedN("Group Pre-submit");
            auto* signal = Construct<RHIDeviceQueue::TimelinePair>(mExecuteAlloc.Ptr());
            if (group.queue == RHIDeviceQueueType::Graphics)
                *signal = RHIDeviceQueue::TimelinePair(mGraphicsTimeline.Get(), Counter(group.graphicsGroupIndex));
            else
                *signal = RHIDeviceQueue::TimelinePair(mComputeTimeline.Get(), Counter(group.computeGroupIndex));
            // Submissions on the same queue are guaranteed to be ordered only by the barriers
            // where ones w/o dependency may _execute_ concurrently in the driver
            // However, cross-queue synchronization is still needed - we'll do timeline semaphores for that
            // https://www.lunarg.com/wp-content/uploads/2021/08/Vulkan-Synchronization-SIGGRAPH-2021.pdf
            auto* wait = Construct<Vector<RHIDeviceQueue::TimelinePair>>(mExecuteAlloc.Ptr(), mExecuteAlloc.Ptr());
            auto* waitStage = Construct<Vector<RHIPipelineStage>>(mExecuteAlloc.Ptr(), mExecuteAlloc.Ptr());
            RHIPipelineStage allStages{};
            for (auto pass_handle : group.passes)
            {
                auto const& pass = mSetup->trackedPasses[pass_handle];
                allStages |= pass.piplineStages;
            }
            if (mSetup->executionNumGraphicsGroups && graphicsWaitValue >= 0 &&
                group.queue == RHIDeviceQueueType::Compute)
                wait->emplace_back(mGraphicsTimeline.Get(), graphicsWaitValue), waitStage->push_back(allStages);
            if (mSetup->executionNumComputeGroups && computeWaitValue >= 0 &&
                group.queue == RHIDeviceQueueType::Graphics)
                wait->emplace_back(mComputeTimeline.Get(), computeWaitValue), waitStage->push_back(allStages);
            if (needPresent)
            {
                // Last group to submit, and we need to present
                CHECK_MSG(group.queue == RHIDeviceQueueType::Graphics,
                          "FIXME-ExecuteFrame: Last pass ended on a non-Graphics queue");
                waitStage->push_back(allStages | RHIPipelineStageBits::BottomOfPipe);
                auto pPresentSemaphores = ConstructSpan<RHIDeviceSemaphore*>(mExecuteAlloc.Ptr(), 2);
                pPresentSemaphores[0] = mSwaps[mCurrentSync].present.Get();
                pPresentSemaphores[1] = mSwaps[GetSwap()].render.Get();
                mExecuteSubmits->push_back({RHIDeviceQueueType::Graphics,
                                            {.timelineWaits = *wait,
                                             .timelineSignals = {signal, 1},
                                             .waits = pPresentSemaphores.subspan(0, 1),
                                             .waitsStages = *waitStage,
                                             .signals = pPresentSemaphores.subspan(1, 1),
                                             .cmdLists = passCmds}});
            }
            else
            {
                mExecuteSubmits->push_back({group.queue,
                                            {.timelineWaits = *wait,
                                             .timelineSignals = {signal, 1},
                                             .waitsStages = *waitStage,
                                             .cmdLists = passCmds}});
            }
        }
    }
}
void Renderer::EndExecute()
{
    ZoneScoped;
    CHECK_MSG(mState == State::Execute, "Renderer bad state ({}). EndExecute() may only be called once per frame.",
              mState);
    // At this point, all passes have been scheduled to record and submit params are valid.
    // Wait for all recording to finish
    {
        ZoneScopedN("Wait for Records");
        mExecuteThreadPool.Join();
    }
    {
        ZoneScopedN("Submit");
        // Submit all the recorded command lists
        int ctr = 0, lastGraphics = -1, lastCompute = -1;
        for (const auto& qtype : *mExecuteSubmits | std::views::keys)
        {
            if (qtype == RHIDeviceQueueType::Graphics)
                lastGraphics = ctr;
            if (qtype == RHIDeviceQueueType::Compute)
                lastCompute = ctr;
            ctr++;
        }
        ctr = 0;
        for (auto const& [qtype, submits] : *mExecuteSubmits)
        {
            RHIDeviceQueue* q = qtype == RHIDeviceQueueType::Graphics ? mGraphicsQueue : mComputeQueue;
            RHIDeviceFence* f = nullptr;
            if (ctr == lastGraphics)
                f = mSwaps[mCurrentSync].graphicsFence.Get();
            if (ctr == lastCompute)
                f = mSwaps[mCurrentSync].computeFence.Get();
            q->Submit({{submits}}, f);
            ctr++;
        }
    }
    // Present if needed
    if (mDesc.present && mSetup->executionAnyGraphics)
    {
        ZoneScopedN("Present");
        mGraphicsQueue->Present(
            {.imageIndex = GetSwap(), .swapchain = mSwapchain.Get(), .waits = {{mSwaps[GetSwap()].render.Get()}}});
    }
    mCurrentSync = (mCurrentSync + 1) % mFrameSwaps;
    mFrameSwapped++;
    mState = State::PostSetup;
    FrameMark;
}
void Renderer::CmdSetPipeline(PassHandle pass, RHICommandList* cmd) const
{
    CHECK(mState == State::Execute);
    auto& tpass = mSetup->trackedPasses[pass];
    CHECK_MSG(tpass.pso.IsValid(), "Current pass {} has no Pipeline state.", tpass.name);
    cmd->SetPipeline({.pipeline = tpass.pso.Get(),
                      .type = tpass.isComputePass ? RHIDevicePipelineType::Compute : RHIDevicePipelineType::Graphics});
    if (!tpass.pDescriptorSets.empty())
        cmd->BindDescriptorSet(tpass.isComputePass ? RHIDevicePipelineType::Compute : RHIDevicePipelineType::Graphics,
                               tpass.pso.Get(), tpass.pDescriptorSets, 0);
    if (tpass.backbufferUAV)
        CmdBindDescriptorSet(pass, cmd, tpass.backbufferUAV.value(), mSwaps[GetSwap()].viewSet.Get());
}
void Renderer::CmdBindDescriptorSet(PassHandle pass, RHICommandList* cmd, uint32_t set_index,
                                    RHIDeviceDescriptorSet* descriptor_set) const
{
    CHECK(mState == State::Execute);
    auto& tpass = mSetup->trackedPasses[pass];
    CHECK_MSG(tpass.pso.IsValid(), "Current pass has no Pipeline state.");
    cmd->BindDescriptorSet(tpass.isComputePass ? RHIDevicePipelineType::Compute : RHIDevicePipelineType::Graphics,
                           tpass.pso.Get(), {{descriptor_set}}, set_index);
}
void Renderer::CmdBindDescriptorSet(PassHandle pass, RHICommandList* cmd, StringView bind_point,
                                    RHIDeviceDescriptorSet* descriptor_set) const
{
    CHECK(mState == State::Execute);
    auto& tpass = mSetup->trackedPasses[pass];
    CHECK_MSG(tpass.pso.IsValid(), "Current pass has no Pipeline state.");
    auto it = std::lower_bound(tpass.externalBindings.begin(), tpass.externalBindings.end(), bind_point,
                               [](auto const& a, auto const& b) { return std::get<0>(a) < b; });
    CHECK_MSG(it != tpass.externalBindings.end() && std::get<0>(*it) == bind_point,
              "No external binding point named '{}' found on pass {}", bind_point, tpass.name);
    int set = std::get<2>(*it);
    return CmdBindDescriptorSet(pass, cmd, set, descriptor_set);
}
void Renderer::CmdBeginGraphics(PassHandle pass, RHICommandList* cmd, RHIExtent2D const& extent,
                                Span<const Optional<RHIClearColor>> clear_rtv,
                                Optional<RHIClearDepthStencil> const& clear_dsv)
{
    CHECK(mState == State::Execute);
    auto& tpass = mSetup->trackedPasses[pass];
    Vector<RHICommandList::GraphicsDesc::Attachment> rtvs(mExecuteAlloc.Ptr());
    rtvs.reserve(tpass.rtvs.size() + 1);
    const size_t rtv_count = tpass.rtvs.size() + (tpass.backbufferRTV ? 1 : 0);
    CHECK_MSG(clear_rtv.size() == rtv_count, "RTV clear count mismatch. Got {}, expected {} for all RenderTargets", clear_rtv.size(), rtv_count);
    if (tpass.backbufferRTV)
    {
        rtvs.push_back({.imageView = mSwaps[GetSwap()].view.Get(), .clearColor = clear_rtv[0]});
        clear_rtv = clear_rtv.subspan(1);
    }
    for (int i = 0; const auto& rtv : tpass.rtvs | std::views::keys)
    {
        auto const& [rhdl, desc] = mSetup->trackedViews[rtv];
        auto const& tres = mSetup->trackedResources[rhdl];
        RHITexture* res = DerefResource(rhdl).Get<RHITexture*>();
        CHECK_MSG(res->mDesc.extent.x >= extent.x && res->mDesc.extent.y >= extent.y,
                  "Graphics extent too large for Render Target on {}", tres.name);
        rtvs.push_back({.imageView = DerefTextureView(rtv), .clearColor = clear_rtv[i]});
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
    fmt::format_to(std::back_inserter(out), "    rankdir=TB; splines=ortho;\n");
    auto& graph = mSetup->graph;
    auto& passes = mSetup->trackedPasses;
    auto& resources = mSetup->trackedResources;
    for (auto& group : mSetup->executionGroups)
    {
        fmt::format_to(std::back_inserter(out), "    subgraph cluster_{} {{\n", group.groupIndex);
        fmt::format_to(std::back_inserter(out), "        label=\"Group {} (Queue={})\";\n", group.groupIndex, group.queue);
        for (auto& pass_handle : group.passes)
        {
            auto& pass = passes[pass_handle];
            fmt::format_to(std::back_inserter(out), "        \"{}@{}\";\n", pass.name, pass.handle);
        }
        fmt::format_to(std::back_inserter(out), "    }}\n");
    }
    for (auto& pass : passes)
    {
        if (pass.handle == mSetup->epilogue) continue;
        fmt::format_to(std::back_inserter(out), "    \"{}@{}\" [ shape=box style={} fillcolor=\"{}\" ];\n", pass.name,
                       pass.handle, pass.used ? "filled" : "unfilled",
                       pass.queue == RHIDeviceQueueType::Graphics ? "#d0e0f0" : "#f0d0e0");
    }
    {
        auto& epilogue = mSetup->trackedPasses[mSetup->epilogue];
        fmt::format_to(std::back_inserter(out), "    \"{}@{}\" [ shape=box style=filled fillcolor=\"#00e000\" ];\n",
                       epilogue.name, epilogue.handle);
    }
    // Dependencies
    for (PassHandle u = 0; u < mSetup->graph.size(); u++)
    {
        for (auto [v, w] : graph[u])
        {
            auto const& resName = w != kInvalidHandle ? resources[w].name : "<Backbuffer or Reserved>";
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
        fmt::format_to(std::back_inserter(out), "{}: {}, depth={}, pri={}, ord={}, queue={}, group={}\n", pass.handle,
                       pass.name, pass.depth, pass.priority, pass.ord, pass.queue, pass.groupIndex);
    }
    out.pop_back();
    return out;
}

String Renderer::DbgDumpExecutionGroups() const
{
    String out;
    for (const auto& group : mSetup->executionGroups)
    {
        fmt::format_to(std::back_inserter(out), "{}: queue={}, passes=[", group.groupIndex, group.queue);
        for (const auto& pass : group.passes)
            fmt::format_to(std::back_inserter(out), "{} ", pass);
        out.pop_back();
        fmt::format_to(std::back_inserter(out), "]\n");
    }
    out.pop_back();
    return out;
}
