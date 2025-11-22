enum FEditorState
{
    FEInit,
    FERunning
} FEState;
// -- Scene data
struct FEMesh
{
    uint32_t offset;
    GSMesh mesh;
    SharedFuture<> future;
};
Vector<FEMesh> FEMeshes(GLOBAL_ALLOC);
Vector<GSInstance> GSInstances(GLOBAL_ALLOC);
uint32_t GSFirstInstance = 0;

#pragma pack(push, 1)
struct MeshletTaskDispatch // VkDrawMeshTasksIndirectCommandEXT
{
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;
};
#pragma pack(pop)
constexpr size_t kMaxMeshletTaskDispatchCount = 1e5;
void RendererSetup(FContext* context)
{
    if (context->renderer)
        Destruct(context->allocator, context->renderer);
    auto* renderer = context->renderer = Construct<Renderer>(context->allocator, RendererDesc{}, context->device,
                                                             context->swapchain, context->allocator);
    auto* scene = context->gpuScene;
    renderer->BeginSetup();
    /* Instance and Primitive buffers */
    auto InstanceBuffer = renderer->CreateResource("Instance Buffer", scene->GetInstanceBuffer());
    auto PrimitiveBuffer = renderer->CreateResource("Primitive Buffer", scene->GetPrimitiveBuffer());
    /* Indirect Task Buffers */
    using enum RHIBufferUsageBits;
    auto IndirectTasks =
        renderer->CreateResource("Meshlet Indirect Tasks Buffer", // Instance IDs
                                 RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer,
                                               .size = sizeof(uint32_t) * kMaxMeshletTaskDispatchCount});
    auto IndirectTaskCounter = renderer->CreateResource(
        "Meshlet Indirect Tasks Counter (Single)",
        RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer | TransferDestination, .size = sizeof(int)});
    // This only contain *one* dispatch command, which can spawn quite enough meshlet draws already!
    // See respective shaders for more details.
    auto IndirectTaskDispatch = renderer->CreateResource(
        "Meshlet Task Indirect Dispatch Command (Single)",
        RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer, .size = sizeof(MeshletTaskDispatch)});
    renderer->CreatePass(
        "Meshlet Task Generation", RHIDeviceQueueType::Compute, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main", "data/shaders/ECSInstanceTaskCull.spv");
            r->BindBufferUnordered(self, IndirectTasks, RHIPipelineStageBits::ComputeShader, "task");
            r->BindBufferUnordered(self, IndirectTaskCounter, RHIPipelineStageBits::ComputeShader, "counter");
            r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instance");
        },
        [&](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            auto* counter = r->DerefResource(IndirectTaskCounter).Get<RHIBuffer*>();
            // Reset counter
            cmd->FillBuffer(counter, 0, 0);
            r->CmdSetPipeline(self, cmd);
            r->CmdDispatch(self, cmd, {GSInstances.size(), 1, 1});
        });
    renderer->CreatePass(
        "Indirect Task Command Generation", RHIDeviceQueueType::Compute, 0u,
        [=](PassHandle self, Renderer* r)
        {
            // Simply fills the dispatch buffer with the number of tasks to dispatch
            // A roundtrip back to the CPU would be expensive, so we do it all on the GPU side.
            r->BindShader(self, RHIShaderStageBits::Compute, "main", "data/shaders/ECSIndirectTaskGen.spv");
            r->BindBufferStorageRead(self, IndirectTaskCounter, RHIPipelineStageBits::ComputeShader, "counter");
            r->BindBufferUnordered(self, IndirectTaskDispatch, RHIPipelineStageBits::ComputeShader, "dispatch");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            r->CmdDispatch(self, cmd, {1, 1, 1});
        });
    /* Meshlet Drawing */
    auto [w, h] = renderer->GetSwapchainExtent();
    auto ZBuffer = renderer->CreateResource("ZBuffer",
                                            RHITextureDesc{.usage = RHITextureUsageBits::DepthStencil,
                                                           .extent = {w, h, 1},
                                                           .format = RHIResourceFormat::D32SignedFloat});
    renderer->CreatePass(
        "Main Pass", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Task, "main", "data/shaders/ETaskMeshletCull.spv");
            r->BindShader(self, RHIShaderStageBits::Mesh, "main", "data/shaders/EMeshBasic.spv");
            r->BindShader(self, RHIShaderStageBits::Fragment, "main", "data/shaders/EFragBasic.spv");
            r->BindBufferShaderRead(self, IndirectTaskDispatch,
                                    RHIPipelineStageBits::DrawIndirect | RHIPipelineStageBits::AllGraphics);
            r->BindBufferStorageRead(self, IndirectTasks, RHIPipelineStageBits::ComputeShader, "task");
            r->BindBufferStorageRead(self, IndirectTaskCounter, RHIPipelineStageBits::ComputeShader, "counter");
            r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instance");
            r->BindBufferStorageRead(self, PrimitiveBuffer,
                                     RHIPipelineStageBits::ComputeShader | RHIPipelineStageBits::AllGraphics,
                                     "sceneShared");
            r->BindBackbufferRTV(self);
            r->BindTextureDSV(self, ZBuffer,
                              {.format = RHIResourceFormat::D32SignedFloat,
                               .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            RHIExtent2D wh{w, h};
            auto* dispatchBuffer = r->DerefResource(IndirectTaskDispatch).Get<RHIBuffer*>();
            r->CmdBeginGraphics(self, cmd, wh, {});
            r->CmdSetPipeline(self, cmd);
            cmd->SetViewport(0, 0, w, h, 0, 1, true).SetScissor(0, 0, w, h);
            cmd->DrawMeshTasksIndirect(dispatchBuffer, 0, 1, sizeof(MeshletTaskDispatch));
            cmd->EndGraphics();
        });
    ImGui_ImplFoundation_CreatePass(renderer, "ImGui", true, FSetupDefault{});
    renderer->EndSetup();
}

bool ExecuteInit()
{
    // Setup Renderer
    RendererSetup(GContext);
    // DEMO: Loading one mesh at startup
    auto* scene = GContext->gpuScene;
    FMesh src(GLOBAL_ALLOC);
    src.Load("data/assets/bunny.obj");
    src.ClusterizeDAG();
    FEMesh mesh;
    mesh.future = scene->Upload(src, mesh.mesh, mesh.offset);
    // Wait for upload to complete
    // This is async and can be waited on later, but for simplicity we just
    // block here.
    mesh.future.wait();
    LOG(Editor, LogInfo, "Uploaded mesh with {} vertices, {} groups, {} LODs", mesh.mesh.vtxCount, mesh.mesh.groupCount,
        mesh.mesh.lodCount);
    FEMeshes.emplace_back(mesh);
    // Create instances
    GSInstances.resize(100);
    for (int i = 0; auto& ins : GSInstances)
    {
        ins.tag = MakeGSInstanceTag(GSDataBits::Mesh, i++);
        ins.data1 = mesh.offset;
    }
    FEState = FERunning;
    return false;
}
bool ExecuteFrame()
{
    auto* renderer = GContext->renderer;
    if (GContext->event.type == SDL_EVENT_WINDOW_RESIZED)
    {
        // Reset renderer to update framebuffer size changes
        // TODO: This *also* recompiles all PSOs. We should cache these.
        RendererSetup(GContext);
    }
    // Upload instance data
    auto* scene = GContext->gpuScene;
    auto [ptr, off] = scene->InstanceAlloc(GSInstances.size());
    std::memcpy(ptr, GSInstances.data(), GSInstances.size() * sizeof(GSInstance));
    GSFirstInstance = off;
    // New frame
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame(&GContext->event);
    ImGui::NewFrame();
    renderer->ExecuteFrame();
    renderer->EndExecute();
    return false;
}
// Per-frame logic
// Executes once a swap is acquired
bool ShouldEditorClose(FContext*)
{
    switch (FEState)
    {
    case FEInit:
        return ExecuteInit();
    case FERunning:
        return ExecuteFrame();
    }
    return false;
}
