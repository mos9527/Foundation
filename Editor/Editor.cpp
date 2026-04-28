#include <cmath>
#include "EditorState.hpp"

// File-local renderer state: written in FRunningEnter, consumed in FRunning/FRendering
static PTReadbackHandles sPTReadback;
static RHIBuffer*        sPickResultBuffer = nullptr;
static int2              sPendingPickPixel{-1, -1};

static float ApertureRadiusFromFStop(float fStop, float sensorHeightMm, float fovY)
{
    if (fStop <= 0.0f || sensorHeightMm <= 0.0f)
        return 0.0f;

    float sensorHeightMeters = sensorHeightMm * 1e-3f;
    float focalLengthMeters = (0.5f * sensorHeightMeters) / std::tan(fovY * 0.5f);
    return focalLengthMeters / (2.0f * fStop);
}

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
    GEditor.state = FEInit;
}

/* ==================== FInit ==================== */
static void FInit()
{
    // Transition to FERunningEnter when scene data is available
    if (!GEditor.doc.instances.empty())
        GEditor.state = FERunningEnter;
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
        if (GEditor.showImGui)
        {
            EditorDockSpaceAndMenuBar();
            FHierarchyPanel();
            FLightingPanel();
        }
        float dt = ImGui::GetIO().DeltaTime;
        bool camMoved = false;
        camMoved |= GEditor.camera.UpdateMovement(dt);
        GEditor.camera.Update({});
        GEditor.camera.aspect = GContext->swapchain->GetAspectRatio();
        renderer->ExecuteFrame();
        renderer->EndExecute();
    }
}

/* ==================== FRunningEnter ==================== */
static void FRunningEnter()
{
    // Renderer setup owns mode-specific render graph resources. Drop the old graph
    // before recreating the swapchain and building the new mode.
    sPickResultBuffer = nullptr;
    if (GContext->renderer)
    {
        Destruct(GContext->allocator, GContext->renderer);
        GContext->renderer = nullptr;
    }
    UpdateSwapchain(GContext);
    // Invalidate stale PT readback handles before rebuilding the renderer
    sPTReadback = {};
    RendererScene scene{
        .gsGlobals = &GEditor.shaderGlobals,
        .gsInstances = &GEditor.doc.instances,
        .gsMaterials = &GEditor.doc.materials,
        .gsMeshes = &GEditor.doc.meshes,
        .gsBLASes = &GEditor.doc.blases,
        .gsLights = &GEditor.doc.lights,
        .gsPickPixel = &sPendingPickPixel
    };
    if (GEditor.rendererMode == ERendererMode::PathTracer)
    {
        PathTracerSetup(GContext, GEditor.rendererConfig, scene, sPTReadback);
        sPickResultBuffer = GContext->renderer->DerefResource(sPTReadback.pickResultBuffer).Get<RHIBuffer*>();
    }
    else
    {
        RasterReadbackHandles rasterHandles;
        RendererSetup(GContext, GEditor.rendererConfig, scene, rasterHandles);
        sPickResultBuffer = GContext->renderer->DerefResource(rasterHandles.pickResultBuffer).Get<RHIBuffer*>();
    }
    GEditor.state = FERunning;
}

/* ==================== FRunning ==================== */
static void FRunning()
{
    auto* renderer = GContext->renderer;
    // New frame
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();
    if (GEditor.showImGui)
    {
        EditorDockSpaceAndMenuBar();
        FHierarchyPanel();
        FRunningImGui();
    }
    // Global param update
    float dt = ImGui::GetIO().DeltaTime;
    GEditor.cameraUpdated |= GEditor.camera.UpdateMovement(dt);
    GEditor.camera.Update({});
    GEditor.camera.aspect = GContext->swapchain->GetAspectRatio();
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
        ? ApertureRadiusFromFStop(GEditor.aperture.fStop, GEditor.aperture.sensorHeightMm, GEditor.camera.fovY)
        : 0.0f;
    GEditor.shaderGlobals.fbWidth = static_cast<float>(renderer->GetSwapchainExtent().x);
    GEditor.shaderGlobals.fbHeight = static_cast<float>(renderer->GetSwapchainExtent().y);
    
    // Sync HDR parameters
    GEditor.shaderGlobals.enableHDR = GContext->enableHDR ? 1 : 0;
    // paperWhiteNits is updated directly in EditorPanels.cpp

    if (GEditor.cameraUpdated)
        GEditor.shaderGlobals.ptAccumulatedFrames = 0, GEditor.cameraUpdated = false;
    renderer->ExecuteFrame();
    renderer->EndExecute();
    // GPU picking: Blit PS wrote pickResult[0] this frame if a click was pending.
    // Readback buffer is coherent+persistently mapped — just read it directly.
    if (sPendingPickPixel.x >= 0 && sPickResultBuffer)
    {
        uint32_t id = *sPickResultBuffer->Map<uint32_t>();
        GEditor.doc.selectedInstance = (id == ~0u) ? -1 : static_cast<int>(id);
        if (GEditor.doc.selectedInstance >= 0 &&
            GEditor.doc.selectedInstance < static_cast<int>(GEditor.doc.instances.size()))
        {
            GEditor.doc.selectedMaterial = static_cast<int>(GEditor.doc.instances[GEditor.doc.selectedInstance].materialIndex);
            GEditor.doc.selectedLight = -1;
        }
        else
        {
            GEditor.doc.selectedMaterial = -1;
        }
        sPendingPickPixel = {-1, -1};
    }
    if (!GEditor.renderTask.renderPaused)
        GEditor.shaderGlobals.ptAccumulatedFrames++;
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
        switch (GEditor.state)
        {
        case FEInit:
            RendererSetupImGuiOnly(GContext);
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
    if (!io.WantCaptureMouse && !ImGuizmo::IsUsing())
        GEditor.cameraUpdated |= GEditor.camera.Update(*event);
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

/* ==================== Per-frame dispatch ==================== */
bool EditorOnFrame(FContext* context)
{
    ResetEditorFrameScratch(context);
    switch (GEditor.state)
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