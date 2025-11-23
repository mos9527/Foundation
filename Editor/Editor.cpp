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

#pragma pack(push, 1)
struct MeshletTaskDispatch // VkDrawMeshTasksIndirectCommandEXT
{
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;
};
struct MeshletTaskWork
{
    uint32_t instanceID; // Absolute
    // Task shader can *only* dispatch zero or one meshlet per work thread.
    // Hence, batching is required here. Whereas in Mesh Shader multiple verts/tris
    // can be processed per thread.
    uint32_t firstMeshlet;
    uint32_t numMeshlets;
};
struct UBO
{
    uint32_t firstInstance;
    uint32_t numInstances;
    float lodThreshold{0.1f};
    float zNear;
    float4x4 view;
    float4x4 proj;
} GShaderGlobals;

#pragma pack(pop)
constexpr size_t kMaxMeshletTaskWorkCount = 1e5;
void RendererSetup(FContext* context)
{
    if (context->renderer)
        Destruct(context->allocator, context->renderer);
    auto* renderer = context->renderer = Construct<Renderer>(context->allocator, RendererDesc{}, context->device,
                                                             context->swapchain, context->allocator);
    auto* scene = context->gpuScene;
    renderer->BeginSetup();
    /* UBO for everyone */
    auto GlobalUBO = renderer->CreateResource(
        "Global UBO",
        RHIBufferDesc{.usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::UniformBuffer,
                      .size = sizeof(UBO)});
    /* Instance and Primitive buffers */
    auto InstanceBuffer = renderer->CreateResource("Instance Buffer", scene->GetInstanceBuffer());
    auto PrimitiveBuffer = renderer->CreateResource("Primitive Buffer", scene->GetPrimitiveBuffer());
    /* Indirect Task Buffers */
    using enum RHIBufferUsageBits;
    auto IndirectTasks =
        renderer->CreateResource("Meshlet Indirect Tasks Buffer", // Instance IDs
                                 RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer,
                                               .size = sizeof(MeshletTaskWork) * kMaxMeshletTaskWorkCount});
    auto IndirectTaskCounter = renderer->CreateResource(
        "Meshlet Indirect Tasks Counter (Single)",
        RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer | TransferDestination, .size = sizeof(int)});
    // This only contain *one* dispatch command, which can spawn quite enough meshlet draws already!
    // See respective shaders for more details.
    auto IndirectTaskDispatch = renderer->CreateResource(
        "Meshlet Task Indirect Dispatch Command (Single)",
        RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer, .size = sizeof(MeshletTaskDispatch)});
    // NOTE: Lambda captures
    // NONE of the handle values outlive the renderer. Therefore, ALWAYS capture by value.
    auto* pShaderGlobals = &GShaderGlobals;
    renderer->CreatePass(
        "UBO Update", RHIDeviceQueueType::Compute, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferCopyDst(self, GlobalUBO);
            r->BindBufferCopyDst(self, IndirectTaskCounter);
        },
        [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            auto* ubo = r->DerefResource(GlobalUBO).Get<RHIBuffer*>();
            auto* counter = r->DerefResource(IndirectTaskCounter).Get<RHIBuffer*>();
            // Fill, Update are considered Transfer operations
            // and would require proper barriers - which are automatically handled
            // by the Renderer *inter* passes.
            // Note that usage before a Dispatch, etc, may be valid but is still a ROW hazard.
            // TODO: Document these.
            cmd->UpdateBuffer(ubo, 0, AsBytes(AsSpan(*pShaderGlobals)));
            /// ^^^ Footgun as noted in MeshShaderHierarchicalLOD.cpp ^^^
            cmd->FillBuffer(counter, 0u);
        });
    renderer->CreatePass(
        "Meshlet Task Generation", RHIDeviceQueueType::Compute, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main", "data/shaders/ECSInstanceTaskCull.spv");
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindBufferUnordered(self, IndirectTasks, RHIPipelineStageBits::ComputeShader, "tasks");
            r->BindBufferUnordered(self, IndirectTaskCounter, RHIPipelineStageBits::ComputeShader, "counter");
            r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instances");
            r->BindBufferStorageRead(self, PrimitiveBuffer,RHIPipelineStageBits::ComputeShader, "primitive");
        },
        [&](PassHandle self, Renderer* r, RHICommandList* cmd)
        {                        
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
            r->BindShader(self, RHIShaderStageBits::Task, "main", "data/shaders/ETSMeshletCull.spv");
            r->BindShader(self, RHIShaderStageBits::Mesh, "main", "data/shaders/EMSBasic.spv");
            r->BindShader(self, RHIShaderStageBits::Fragment, "main", "data/shaders/EPSBasic.spv");
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindBufferShaderRead(self, IndirectTaskDispatch, RHIPipelineStageBits::AllGraphics | RHIPipelineStageBits::DrawIndirect);
            r->BindBufferStorageRead(self, IndirectTasks, RHIPipelineStageBits::ComputeShader, "tasks");
            r->BindBufferStorageRead(self, IndirectTaskCounter, RHIPipelineStageBits::ComputeShader, "counter");
            r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instances");
            r->BindBufferStorageRead(self, PrimitiveBuffer, RHIPipelineStageBits::AllGraphics, "primitive");
            r->BindBackbufferRTV(self);
            r->BindTextureDSV(self, ZBuffer,
                              {.format = RHIResourceFormat::D32SignedFloat,
                               .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            RHIExtent2D wh{w, h};
            auto* dispatchBuffer = r->DerefResource(IndirectTaskDispatch).Get<RHIBuffer*>();
            r->CmdBeginGraphics(self, cmd, wh, RHIClearColor{}, RHIClearDepthStencil{0.0f, 0});
            r->CmdSetPipeline(self, cmd);
            cmd->SetViewport(0, 0, w, h, 0, 1, true).SetScissor(0, 0, w, h);
            cmd->DrawMeshTasksIndirect(dispatchBuffer, 0, 1, sizeof(MeshletTaskDispatch));
            cmd->EndGraphics();
        });
    ImGui_ImplFoundation_CreatePass(renderer, "ImGui", false, FSetupDefault{});
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
    LOG(Editor, LogInfo, "Uploaded mesh with {} vertices, {} groups, {} meshlets", mesh.mesh.vtxCount, mesh.mesh.groupCount,
        mesh.mesh.meshletCount);
    FEMeshes.emplace_back(mesh);
    // Create instances
    float sz = 10, scale = 1.0f;
    GSInstances.resize(sz * sz * sz);
    for (int x = 0; x < sz; x++)
        for (int y = 0; y < sz; y++)
            for (int z = 0; z < sz; z++)
            {
                int i = x * sz * sz + y * sz + z;
                GSInstances[i].data[0] = mesh.offset;
                GSInstances[i].transform = float3{(x - sz / 2), (y - sz / 2), (z - sz / 2)} * scale;
            }
    FEState = FERunning;
    return false;
}

struct ArcballCamera
{
    static constexpr char kControlsText[] = "Mouse Left: Rotate | Mouse Right: Pan | Mouse Wheel: Zoom";

    float3 center;
    float radius;
    float pitch, yaw;
    float zNear, fovY, aspect;
    mat4 view, proj;
    mat4 Update(SDL_Event const& event)
    {
        if (event.type == SDL_EVENT_MOUSE_MOTION)
        {
            if (event.motion.state & SDL_BUTTON_LMASK)
            {
                pitch -= event.motion.xrel * 1e-2f;
                yaw -= event.motion.yrel * 1e-2f;
            }
            if (event.motion.state & SDL_BUTTON_RMASK)
            {
                quat rot = angleAxis(yaw, vec3(1, 0, 0)) * angleAxis(pitch, vec3(0, 1, 0));
                vec3 right = rot * vec3(1, 0, 0);
                vec3 up = rot * vec3(0, 1, 0);
                center -= right * (event.motion.xrel * radius * 1e-3f);
                center += up * (event.motion.yrel * radius * 1e-3f);
            }
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL)
        {
            radius -= event.wheel.y * radius * 1e-1f;
            radius = radius < 1e-3f ? 1e-3f : radius;
        }
        // ---
        proj = infinitePerspectiveRHReverseZ(fovY, aspect, zNear);
        quat rot = angleAxis(yaw, vec3(1, 0, 0)) * angleAxis(pitch, vec3(0, 1, 0));
        vec3 dir = rot * vec3(0, 0, 1);
        view = viewMatrixRHReverseZ(center + radius * dir, rot);
        mat4 viewProj = proj * view;
        return viewProj;
    }
};

bool ExecuteFrame()
{
    if (GContext->event.type == SDL_EVENT_WINDOW_RESIZED)
    {
        // Reset renderer to update framebuffer size changes
        // TODO: This *also* recompiles all PSOs. We should cache these.
        RendererSetup(GContext);
    }
    auto* renderer = GContext->renderer;
    auto* scene = GContext->gpuScene;
    // New frame
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame(&GContext->event);
    ImGui::NewFrame();
    // Upload instance data
    auto [ptr, off] = scene->InstanceAlloc(GSInstances.size());
    std::memcpy(ptr, GSInstances.data(), GSInstances.size() * sizeof(GSInstance));
    GShaderGlobals.firstInstance = off;
    GShaderGlobals.numInstances = GSInstances.size();
    // Global param update
    static ArcballCamera camera{
        .center = float3{0, 0, 0},
        .radius = 2.5f,
        .pitch = 0.f,
        .yaw = 0.f,
        .zNear = 0.1f,
        .fovY = radians(60.f),
    };
    camera.aspect = GContext->swapchain->GetAspectRatio();
    auto& io = ImGui::GetIO();
    if (!io.WantCaptureMouse)
        camera.Update(GContext->event);
    GShaderGlobals.view = camera.view;
    GShaderGlobals.proj = camera.proj;
    GShaderGlobals.zNear = camera.zNear;
    ImGui::Begin("Debug");
    ImGui::TextUnformatted(ArcballCamera::kControlsText);
    ImGui::Text("FPS | %.2f", ImGui::GetIO().Framerate);
    ImGui::Text("StreamingPool | %s", scene->DbgGetStatistics().c_str());
    ImGui::Text("Instances | %zu", GSInstances.size());
    ImGui::SliderFloat("LOD Threshold | ", &GShaderGlobals.lodThreshold, 0.f, 1.f);
    ImGui::End();
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
