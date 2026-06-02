#include <cmath>
#include "EditorState.hpp"
#include <ImGuizmo.h>

EditorState GEditor;

// File-local renderer state: written in FRunningEnter, consumed in FRunning/FRendering
static RendererHandles sRenderReadback;
static RHIBuffer*        sPickResultBuffer = nullptr;
static RendererPicking   sPicking;

static RHIExtent2D ClampViewportExtent(RHIExtent2D extent)
{
    return {std::max(extent.x, 16u), std::max(extent.y, 16u)};
}

static RHIExtent2D ViewportExtentFromImGui()
{
    ImGuiIO& io = ImGui::GetIO();
    uint32_t w = static_cast<uint32_t>(std::round(std::max(io.DisplaySize.x * io.DisplayFramebufferScale.x, 16.0f)));
    uint32_t h = static_cast<uint32_t>(std::round(std::max(io.DisplaySize.y * io.DisplayFramebufferScale.y, 16.0f)));
    return {w, h};
}

static void UpdateBackbufferViewport()
{
    ImGuiIO& io = ImGui::GetIO();
    GEditor.viewport.contentMin = ImVec2(0.0f, 0.0f);
    GEditor.viewport.contentMax = io.DisplaySize;
    GEditor.viewport.renderExtent = ViewportExtentFromImGui();
    GEditor.viewport.visible = io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f;
}

void DestroyEditorRenderer(FContext* context)
{
    context = context ? context : GContext;
    if (!context || !context->renderer)
        return;

    Destruct(context->allocator, context->renderer);
    context->renderer = nullptr;
}

static Renderer* BeginEditorRendererSetup(FContext* context, uint32_t threadCount)
{
    DestroyEditorRenderer(context);

    RendererDesc desc{};
    desc.asyncCompute = true;
    desc.threadCount = threadCount;
    desc.pipelineCache = context->psoCache.Get();
    auto* renderer = context->renderer = Construct<Renderer>(context->allocator, desc, context->device,
                                                             context->swapchain, context->allocator);
    renderer->BeginSetup();
    return renderer;
}

static void EndEditorRendererSetup(Renderer* renderer)
{
    ImGui_ImplFoundation_CreatePass(renderer, "ImGui", false, FSetupDefault{});
    renderer->EndSetup();
}

static void SetupIdleRenderer(FContext* context)
{
    auto* renderer = BeginEditorRendererSetup(context, 0u);
    EndEditorRendererSetup(renderer);
}

static void SetupSceneRenderer(FContext* context, RendererScene scene, RendererHandles& outHandles)
{
    auto* renderer = BeginEditorRendererSetup(context, 4u);
    RHIExtent2D renderExtent = ClampViewportExtent(renderer->GetSwapchainExtent());
    GEditor.viewport.renderExtent = renderExtent;
    GEditor.rendererConfig.enableHDR = context->enableHDR;
    if (GEditor.rendererMode == ERendererMode::PathTracer)
        BuildPathTracerRenderGraph(renderer, context->gpuScene, GEditor.rendererConfig, scene, renderExtent, outHandles,
                                   &GEditor.renderTask.renderPaused);
    if (GEditor.rendererMode == ERendererMode::Raster)
        BuildRasterRenderGraph(renderer, context->gpuScene, GEditor.rendererConfig, scene, renderExtent, outHandles);
    EndEditorRendererSetup(renderer);
}

static void FInitEnter()
{
    // Loading a scene installs it immediately and switches to FERunningEnter (it renders
    // while it streams in). If no file installs a scene, fall back to the no-scene branch.
    for (size_t i = 0; i < GContext->files.size(); i++)
        HandleFile(GContext->files[i]);
    if (GEditor.state == FEInitEnter)
    {
        LOG(Editor, LogInfo, "No scene path provided, starting with empty editor");
        SetupIdleRenderer(GContext);
        GEditor.state = FENoScene;
    }
}

// No scene installed yet: render the ImGui shell only. A drag-and-drop / file open installs
// a scene and transitions to FERunningEnter.
static void FNoScene()
{
    auto* renderer = GContext->renderer;
    if (!renderer)
    {
        SetupIdleRenderer(GContext);
        renderer = GContext->renderer;
    }
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();
    if (GEditor.showImGui)
    {
        EditorDockSpaceAndMenuBar();
        FHierarchyPanel();
        FLightingPanel();
    }
    float dt = ImGui::GetIO().DeltaTime;
    GEditor.camera.aspect = GContext->swapchain->GetAspectRatio();
    GEditor.camera.UpdateMovement(dt);
    GEditor.camera.Update({});
    renderer->ExecuteFrame();
    renderer->EndExecute();
}

static void FRunningEnter()
{
    sPickResultBuffer = nullptr;
    sPicking.pendingPixel = {-1, -1};
    // The old renderer owns views into the current swapchain; release them before recreating the swapchain.
    DestroyEditorRenderer(GContext);
    UpdateSwapchain(GContext);
    // Invalidate stale readback handles before rebuilding the renderer
    sRenderReadback = {};
    // Build the TLAS up front so the render graph captures a valid, correctly-sized TLAS
    // (a full build grows the TLAS buffers; the per-frame pass then only refits).
    if (GContext->gpuScene)
    {
        ImmediateContext tlasCtx(RHIDeviceQueueType::Compute, GContext->device.Get());
        auto* cmd = tlasCtx.Get();
        cmd->Begin();
        auto tlasResult = GContext->gpuScene->BuildTLAS(cmd, false);
        cmd->End();
        if (tlasResult == GPUScene::TLASBuildResult::Built)
            tlasCtx.Submit(), tlasCtx.WaitIdle();
    }
    RendererScene scene{
        .gsGlobals = &GEditor.shaderGlobals,
        .picking = &sPicking,
    };
    SetupSceneRenderer(GContext, scene, sRenderReadback);
    sPickResultBuffer = GContext->renderer->DerefResource(sRenderReadback.pickBuffer).Get<RHIBuffer*>();
    GEditor.state = FERunning;
}

static void FRunning()
{
    auto* renderer = GContext->renderer;
    // New frame
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();
    UpdateBackbufferViewport();
    if (GEditor.showImGui)
    {
        EditorDockSpaceAndMenuBar();
        FHierarchyPanel();
        FRunningImGui();
    }
    // Global param update
    float dt = ImGui::GetIO().DeltaTime;
    RHIExtent2D renderExtent = ClampViewportExtent(GEditor.viewport.renderExtent);
    GEditor.camera.aspect = static_cast<float>(renderExtent.x) / static_cast<float>(renderExtent.y);
    GEditor.cameraUpdated |= GEditor.camera.UpdateMovement(dt);
    GEditor.camera.Update({});
    GEditor.shaderGlobals.frameNumber = renderer->GetFrame();
    GEditor.shaderGlobals.view = GEditor.camera.view;
    GEditor.shaderGlobals.proj = GEditor.camera.proj;
    GEditor.shaderGlobals.inverseView = inverse(GEditor.shaderGlobals.view);
    GEditor.shaderGlobals.inverseViewProj = inverse(GEditor.shaderGlobals.proj * GEditor.shaderGlobals.view);
    GEditor.shaderGlobals.zNear = GEditor.camera.zNear;
    GEditor.shaderGlobals.projPlanes = planeSymmetric(GEditor.shaderGlobals.proj);

    GEditor.shaderGlobals.camPosition = float4(GEditor.camera.position, 0);
    GEditor.shaderGlobals.camDirection = float4(GEditor.camera.rot * float3(0, 0, -1), 0);
    GEditor.shaderGlobals.aperture = GEditor.aperture.dofEnabled
        ? ApertureRadiusFromFStop(GEditor.aperture.fStop, GEditor.aperture.sensorHeightMm * 1e-3f,
                                  GEditor.camera.fovY)
        : 0.0f;
    GEditor.shaderGlobals.fbWidth = static_cast<float>(renderExtent.x);
    GEditor.shaderGlobals.fbHeight = static_cast<float>(renderExtent.y);
    GEditor.shaderGlobals.ptViewFlags = GEditor.rendererConfig.viewFlags;
    GEditor.shaderGlobals.energyCompensation = GEditor.rendererConfig.energyCompensation ? 1u : 0u;

    // Skinned animation playback: evaluate poses and CPU-skin into the dynamic ring, then re-author
    // the scene so dynamic instances encode the current ring slot (the graph's BLAS Update pass
    // refits them). Paused/held poses skip this so the path tracer keeps accumulating.
    if (UpdateAnimation(dt))
        CommitSceneToGPU(true);

    // -- AutoPause: any "user operation" exits AutoPaused. We define a user operation
    //    as anything that resets the path-tracer accumulation, which covers camera
    //    movement (cameraUpdated) and any UI control change that sets
    //    ptAccumulatedFrames = 0 (PT settings, light edits, etc.). A drop in
    //    ptAccumulatedFrames since the last frame is the canonical signal.
    static uint32_t sPrevPTAccumulatedFrames = 0u;
    bool ptAccumWasReset = GEditor.shaderGlobals.ptAccumulatedFrames < sPrevPTAccumulatedFrames;
    bool userOperation = GEditor.cameraUpdated || ptAccumWasReset;
    if (userOperation && GEditor.renderTask.renderAutoPaused)
    {
        GEditor.renderTask.renderAutoPaused = false;
        GEditor.renderTask.renderPaused = false;
    }
    if (GEditor.cameraUpdated)
        GEditor.shaderGlobals.ptAccumulatedFrames = 0, GEditor.cameraUpdated = false;
    renderer->ExecuteFrame();
    renderer->EndExecute();
    // GPU picking: Blit PS wrote pickResult[0] this frame if a click was pending.
    // Readback buffer is coherent+persistently mapped — just read it directly.
    if (sPicking.pendingPixel.x >= 0 && sPickResultBuffer)
    {
        uint32_t id = *sPickResultBuffer->Map<uint32_t>();
        GEditor.selectedInstance = -1;
        GEditor.selectedMaterial = -1;
        // The pick id is a TLAS instanceID, which equals the committed instance index
        // (the editor commits instances 1:1 in scene-row order).
        uint32_t picked = (id != ~0u && GContext->gpuScene)
            ? GContext->gpuScene->ResolvePickedInstance(id)
            : UINT32_MAX;
        if (picked != UINT32_MAX)
        {
            GEditor.selectedInstance = static_cast<int>(picked);
            GEditor.selectedMaterial = static_cast<int>(GContext->gpuScene->GetInstance(picked).materialIndex);
            GEditor.selectedLight = -1;
        }
        sPicking.pendingPixel = {-1, -1};
    }
    if (!GEditor.renderTask.renderPaused)
        GEditor.shaderGlobals.ptAccumulatedFrames += PTSamplesPerDispatch(GEditor.shaderGlobals);

    // -- AutoPause trigger: only meaningful in PathTracer mode with a positive limit.
    //    Once enough pixel samples have accumulated, transition Running -> AutoPaused.
    //    (Manual pause is left untouched.)
    if (GEditor.rendererMode == ERendererMode::PathTracer &&
        !GEditor.renderTask.renderPaused &&
        GEditor.renderTask.autoPauseSampleLimit > 0)
    {
        uint32_t completed = PTCompletedPixelSamples(GEditor.shaderGlobals);
        if (completed >= static_cast<uint32_t>(GEditor.renderTask.autoPauseSampleLimit))
        {
            GEditor.renderTask.renderAutoPaused = true;
            GEditor.renderTask.renderPaused = true;
        }
    }
    sPrevPTAccumulatedFrames = GEditor.shaderGlobals.ptAccumulatedFrames;
}

static ImVec2 EventMousePosition(SDL_Event const& event)
{
    switch (event.type)
    {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return {event.button.x, event.button.y};
    case SDL_EVENT_MOUSE_MOTION:
        return {event.motion.x, event.motion.y};
    default:
        return ImGui::GetIO().MousePos;
    }
}

static bool ViewportAcceptsMouse(SDL_Event const& event)
{
    ImGuiIO& io = ImGui::GetIO();
    UpdateBackbufferViewport();
    ImVec2 pos = EventMousePosition(event);
    if (!GEditor.viewport.visible || !GEditor.viewport.Contains(pos))
        return false;
    return !io.WantCaptureMouse || !GEditor.showImGui;
}

bool EditorProcessEvent(SDL_Event* event)
{
    // Handle drag-and-drop file events
    if (event->type == SDL_EVENT_DROP_FILE)
    {
        const char* droppedFile = event->drop.data;
        if (droppedFile)
        {
            LOG(Editor, LogInfo, "File dropped: {}", droppedFile);
            HandleFile(droppedFile);
        }
    }
    bool windowOutputChanged = event->type == SDL_EVENT_WINDOW_RESIZED;
#if SDL_VERSION_ATLEAST(3, 2, 0)
    windowOutputChanged = windowOutputChanged || event->type == SDL_EVENT_WINDOW_HDR_STATE_CHANGED;
#endif
    if (windowOutputChanged)
    {
        switch (GEditor.state)
        {
        case FENoScene:
            SetupIdleRenderer(GContext);
            break;
        case FERunning:
            GEditor.state = FERunningEnter;
            break;
        default:
            break;
        }
    }
    if (event->type == SDL_EVENT_KEY_DOWN)
    {
        if (event->key.key == SDLK_SPACE)
        {
            GEditor.camera.radius = std::max(length(GEditor.camera.center), 1.0f);
            GEditor.camera.center = {};
            GEditor.cameraUpdated |= true;
        }
        if (event->key.key == SDLK_TAB)
        {
            GEditor.showImGui = !GEditor.showImGui;
            GEditor.shaderGlobals.postShowOutline = GEditor.showImGui;
        }
        // Gizmo hotkeys
        if (event->key.key == SDLK_G)
            GEditor.gizmo.op = ImGuizmo::TRANSLATE;
        if (event->key.key == SDLK_R)
            GEditor.gizmo.op = ImGuizmo::ROTATE;
        if (event->key.key == SDLK_Q)
            GEditor.gizmo.op = ImGuizmo::SCALE;
    }
    ImGui_ImplFoundation_ProcessEvent(event);
    auto& io = ImGui::GetIO();
    bool viewportMouse = ViewportAcceptsMouse(*event);
    bool gizmoActive = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
    if (viewportMouse && !gizmoActive)
        GEditor.cameraUpdated |= GEditor.camera.Update(*event);
    // GPU picking: record click pixel on left mouse button release (not dragging)
    if (viewportMouse && !gizmoActive)
    {
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT)
        {
            int2 pixel;
            if (GEditor.viewport.WindowPointToRenderPixel(EventMousePosition(*event), pixel))
                sPicking.pendingPixel = pixel;
        }
    }
    // Always track WASD key state for the camera (only when ImGui does not need the keyboard)
    if (!io.WantCaptureKeyboard)
    {
        if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP)
        {
            bool pressed = (event->type == SDL_EVENT_KEY_DOWN);
            switch (event->key.key)
            {
            case SDLK_W: GEditor.camera.keyW = pressed; break;
            case SDLK_A: GEditor.camera.keyA = pressed; break;
            case SDLK_S: GEditor.camera.keyS = pressed; break;
            case SDLK_D: GEditor.camera.keyD = pressed; break;
            case SDLK_LSHIFT: case SDLK_RSHIFT: GEditor.camera.keyShift = pressed; break;
            default: break;
            }
        }
    }
    else
    {
        // When ImGui wants the keyboard, clear all movement key states to prevent stuck keys
        GEditor.camera.keyW = GEditor.camera.keyA = GEditor.camera.keyS = GEditor.camera.keyD = GEditor.camera.keyShift = false;
    }
    return false;
}

bool EditorOnFrame(FContext* context)
{
    ResetEditorFrameScratch(context);
    // Finalize/install any scene whose background upload finished this frame.
    PumpSceneLoad();
    switch (GEditor.state)
    {
    case FEInitEnter:
        FInitEnter();
        break;
    case FENoScene:
        FNoScene();
        break;
    case FERunningEnter:
        FRunningEnter();
        break;
    case FERunning:
        FRunning();
        break;
    case FERendering:
        FRendering(sRenderReadback);
        break;
    default:
        return true;
    }
    return false;
}