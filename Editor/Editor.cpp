#include "EditorState.hpp"

// File-local renderer state: written in FRunningEnter, consumed in FRunning/FRendering
static PTReadbackHandles sPTReadback;
static RHIBuffer*        sPickResultBuffer = nullptr;
static int2              sPendingPickPixel{-1, -1};

/* ==================== FInitEnter ==================== */
static void FInitEnter()
{
    if (GContext->files.size() < 1)
    {
        LOG(Editor, LogInfo, "No scene path provided, starting with empty scene");
        RendererSetupImGuiOnly(GContext);
    }
    else
        for (int i = 0; i < GContext->files.size(); i++)
            HandleFile(GContext->files[i]);
    FEState = FEInit;
}

/* ==================== FInit ==================== */
static void FInit()
{
    // Transition to FERunningEnter when scene data is available
    if (!GDoc.instances.empty())
        FEState = FERunningEnter;
    else
    {
        auto* renderer = GContext->renderer;
        if (!renderer)
        {
            RendererSetupImGuiOnly(GContext);
            renderer = GContext->renderer;
        }
        renderer->BeginExecute();
        ImGui_ImplFoundation_NewFrame();
        ImGui::NewFrame();
        if (GShowImGui)
        {
            EditorDockSpaceAndMenuBar();
            FHierarchyPanel();
            FLightingPanel();
        }
        float dt = ImGui::GetIO().DeltaTime;
        bool camMoved = false;
        camMoved |= GCamera.UpdateMovement(dt);
        GCamera.Update({});
        GCamera.aspect = GContext->swapchain->GetAspectRatio();
        renderer->ExecuteFrame();
        renderer->EndExecute();
    }
}

/* ==================== FRunningEnter ==================== */
static void FRunningEnter()
{
    UpdateSwapchain(GContext);
    // Invalidate stale PT readback handles before rebuilding the renderer
    sPTReadback = {};
    RendererScene scene{
        .gsGlobals = &GShaderGlobals,
        .gsInstances = &GDoc.instances,
        .gsMaterials = &GDoc.materials,
        .gsMeshes = &GDoc.meshes,
        .gsBLASes = &GDoc.blases,
        .gsLights = &GDoc.lights,
        .gsPickPixel = &sPendingPickPixel
    };
    if (GRendererMode == ERendererMode::PathTracer)
    {
        PathTracerSetup(GContext, GRendererConfig, scene, sPTReadback);
        sPickResultBuffer = GContext->renderer->DerefResource(sPTReadback.pickResultBuffer).Get<RHIBuffer*>();
    }
    else
    {
        RasterReadbackHandles rasterHandles;
        RendererSetup(GContext, GRendererConfig, scene, rasterHandles);
        sPickResultBuffer = GContext->renderer->DerefResource(rasterHandles.pickResultBuffer).Get<RHIBuffer*>();
    }
    FEState = FERunning;
}

/* ==================== FRunning ==================== */
static void FRunning()
{
    auto* renderer = GContext->renderer;
    // New frame
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();
    if (GShowImGui)
    {
        EditorDockSpaceAndMenuBar();
        FHierarchyPanel();
        FRunningImGui();
    }
    // Global param update
    float dt = ImGui::GetIO().DeltaTime;
    cameraUpdated |= GCamera.UpdateMovement(dt);
    GCamera.Update({});
    GCamera.aspect = GContext->swapchain->GetAspectRatio();
    GShaderGlobals.frameNumber = renderer->GetFrame();
    GShaderGlobals.view = GCamera.view;
    GShaderGlobals.proj = GCamera.proj;
    GShaderGlobals.inverseView = inverse(GShaderGlobals.view);
    GShaderGlobals.inverseViewProj = inverse(GShaderGlobals.proj * GShaderGlobals.view);
    GShaderGlobals.zNear = GCamera.zNear;
    GShaderGlobals.projPlanes = planeSymmetric(GShaderGlobals.proj);

    GShaderGlobals.camPosition = float4(GCamera.position, 0);
    GShaderGlobals.camDirection = float4(GCamera.rot * float3(0, 0, -1), 0);
    GShaderGlobals.fbWidth = static_cast<float>(renderer->GetSwapchainExtent().x);
    GShaderGlobals.fbHeight = static_cast<float>(renderer->GetSwapchainExtent().y);
    
    // Sync HDR parameters
    GShaderGlobals.enableHDR = GContext->enableHDR ? 1 : 0;
    // paperWhiteNits is updated directly in EditorPanels.cpp

    if (cameraUpdated)
        GShaderGlobals.ptAccumulatedFrames = 0, cameraUpdated = false;
    renderer->ExecuteFrame();
    renderer->EndExecute();
    // GPU picking: Blit PS wrote pickResult[0] this frame if a click was pending.
    // Readback buffer is coherent+persistently mapped — just read it directly.
    if (sPendingPickPixel.x >= 0 && sPickResultBuffer)
    {
        uint32_t id = *sPickResultBuffer->Map<uint32_t>();
        GDoc.selectedInstance = (id == ~0u) ? -1 : static_cast<int>(id);
        sPendingPickPixel = {-1, -1};
    }
    if (!GRenderImageTask.renderPaused)
        GShaderGlobals.ptAccumulatedFrames++;
}

/* ==================== Event Processing ==================== */
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
    if (event->type == SDL_EVENT_WINDOW_RESIZED)
    {
        switch (FEState)
        {
        case FEInit:
            RendererSetupImGuiOnly(GContext);
            break;
        case FERunning:
            FEState = FERunningEnter;
            break;
        default:
            break;
        }
    }
    if (event->type == SDL_EVENT_KEY_DOWN)
    {
        if (event->key.key == SDLK_SPACE)
        {
            GCamera.radius = std::max(length(GCamera.center), 1.0f);
            GCamera.center = {};
            cameraUpdated |= true;
        }
        if (event->key.key == SDLK_TAB)
        {
            GShowImGui = !GShowImGui;
            GShaderGlobals.postShowOutline = GShowImGui;
        }
        // Gizmo hotkeys
        if (event->key.key == SDLK_G)
            GGizmo.op = ImGuizmo::TRANSLATE;
        if (event->key.key == SDLK_R)
            GGizmo.op = ImGuizmo::ROTATE;
        if (event->key.key == SDLK_Q)
            GGizmo.op = ImGuizmo::SCALE;
    }
    ImGui_ImplFoundation_ProcessEvent(event);
    auto& io = ImGui::GetIO();
    if (!io.WantCaptureMouse && !ImGuizmo::IsUsing())
        cameraUpdated |= GCamera.Update(*event);
    // GPU picking: record click pixel on left mouse button release (not dragging)
    if (!io.WantCaptureMouse && !ImGuizmo::IsUsing())
    {
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT)
        {
            sPendingPickPixel = {(int)event->button.x, (int)event->button.y};
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
            case SDLK_W: GCamera.keyW = pressed; break;
            case SDLK_A: GCamera.keyA = pressed; break;
            case SDLK_S: GCamera.keyS = pressed; break;
            case SDLK_D: GCamera.keyD = pressed; break;
            case SDLK_LSHIFT: case SDLK_RSHIFT: GCamera.keyShift = pressed; break;
            default: break;
            }
        }
    }
    else
    {
        // When ImGui wants the keyboard, clear all movement key states to prevent stuck keys
        GCamera.keyW = GCamera.keyA = GCamera.keyS = GCamera.keyD = GCamera.keyShift = false;
    }
    return false;
}

/* ==================== Per-frame dispatch ==================== */
bool EditorOnFrame(FContext*)
{
    switch (FEState)
    {
    case FEInitEnter:
        FInitEnter();
        break;
    case FEInit:
        FInit();
        break;
    case FERunningEnter:
        FRunningEnter();
        break;
    case FERunning:
        FRunning();
        break;
    case FERendering:
        FRendering(sPTReadback);
        break;
    default:
        return true;
    }
    return false;
}