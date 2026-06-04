#pragma once
#include <algorithm>
#include <cmath>
#include <Renderer/GPUScene.hpp>
#include <Renderer/Postprocess.hpp>
#include "Camera.hpp"
#include "Editor.hpp"
#include "Scene/Scene.hpp"

enum class ERendererMode
{
    PathTracer,
    Raster
};

enum class ERenderFormat { HDR, SDR };

// Offline render workflow state: flows from file dialog → popup → readback
struct RenderWorkflow
{
    ERenderFormat format       = ERenderFormat::HDR;
    int           targetSamples     = 0;
    int           samplePopupInput  = 4096;
    bool          openRenderPopup   = false;
    // Pause state - kept here so the Path Tracer pass setup can read it via pointer.
    // renderPaused is true whenever rendering should NOT progress (manual OR auto).
    // renderAutoPaused distinguishes the auto-pause state, which is auto-cleared on
    // any user operation. Manual pause (PT button) does NOT auto-clear.
    bool          renderPaused      = false;
    bool          renderAutoPaused  = false;
    // Auto-pause: when > 0, rendering auto-pauses once this many pixel samples
    // have accumulated. Set via Rendering window slider.
    int           autoPauseSampleLimit = 0;
    String        outputPath;
    int           previousSpp = 0, previousSppTile = 0;
};

// Gizmo UI state (matches ImGuizmo::OPERATION and ImGuizmo::MODE)
struct GizmoState
{
    int op, mode;
};

struct CameraApertureState
{
    bool dofEnabled = false;
    float fStop = 2.8f;
    float sensorHeightMm = 36.0f;
};

struct EditorViewportState
{
    ImVec2 contentMin{0.0f, 0.0f};
    ImVec2 contentMax{0.0f, 0.0f};
    RHIExtent2D renderExtent{1280u, 720u};
    bool visible{false};

    ImVec2 Size() const
    {
        return {
            std::max(contentMax.x - contentMin.x, 0.0f),
            std::max(contentMax.y - contentMin.y, 0.0f)
        };
    }

    bool HasRect() const
    {
        ImVec2 size = Size();
        return size.x > 0.0f && size.y > 0.0f;
    }

    bool Contains(ImVec2 pos) const
    {
        return HasRect() &&
            pos.x >= contentMin.x && pos.x < contentMax.x &&
            pos.y >= contentMin.y && pos.y < contentMax.y;
    }

    bool WindowPointToRenderPixel(ImVec2 pos, int2& outPixel) const
    {
        if (!Contains(pos) || renderExtent.x == 0u || renderExtent.y == 0u)
            return false;

        ImVec2 size = Size();
        float u = (pos.x - contentMin.x) / size.x;
        float v = (pos.y - contentMin.y) / size.y;
        int x = static_cast<int>(u * static_cast<float>(renderExtent.x));
        int y = static_cast<int>(v * static_cast<float>(renderExtent.y));
        outPixel = {
            std::clamp(x, 0, static_cast<int>(renderExtent.x) - 1),
            std::clamp(y, 0, static_cast<int>(renderExtent.y) - 1)
        };
        return true;
    }
};

inline float ApertureRadiusFromFStop(float fStop, float sensorHeight, float fovY)
{
    if (fStop <= 0.0f || sensorHeight <= 0.0f)
        return 0.0f;

    float focalLength = (0.5f * sensorHeight) / std::tan(fovY * 0.5f);
    return focalLength / (2.0f * fStop);
}

struct EditorState
{
    // GPUScene owns all scene-data residency (geometry, textures) and the committed
    // instance/material/light tables. The editor keeps the bindings needed to refill
    // those tables from the FSCN scene: resident geometry handles per mesh/curve
    // resource, and the FSCN texture index -> bindless index remap.
    Vector<GeometryHandle> meshGeometry{GLOBAL_ALLOC};
    Vector<GeometryHandle> curveGeometry{GLOBAL_ALLOC};
    Vector<TextureHandle> textureIDMap{GLOBAL_ALLOC};
    Optional<MemoryMappedFile> sceneFile;
    Optional<FImportedScene>     scene;
    String             currentSavePath;
    int                selectedInstance = -1;
    int                selectedMaterial = -1;
    int                selectedLight    = -1;
    RenderWorkflow  renderTask;
    GizmoState      gizmo;
    CameraApertureState aperture;
    EditorViewportState viewport;
    RendererConfig  rendererConfig;
    ERendererMode   rendererMode = ERendererMode::PathTracer;
    int             viewLUTSdrIndex = Postprocess::GetDefaultViewLUTIndex(Postprocess::ViewLUTDomain::SDR);
    int             viewLUTHdrIndex = Postprocess::GetDefaultViewLUTIndex(Postprocess::ViewLUTDomain::HDR);
    String          viewLUTSdrExternalPath;
    String          viewLUTHdrExternalPath;
    TextureHandle   viewLUTSdrHandle{};
    TextureHandle   viewLUTHdrHandle{};

    UBO             shaderGlobals;
    PostprocessUBO  postprocessGlobals;
    FArcballCamera  camera{
        .center = float3{0, 0, 0},
        .radius = 1.0f,
        .zNear = 0.1f,
        .fovY = radians(60.f),
    };
    bool            showImGui = false;
    FEditorState    state = FEInitEnter;
    bool            cameraUpdated = true;

    [[nodiscard]] bool HasScene() const { return scene.has_value(); }
    FImportedScene& Scene()
    {
        CHECK(scene.has_value());
        return *scene;
    }
    FImportedScene const& Scene() const
    {
        CHECK(scene.has_value());
        return *scene;
    }
    void OpenSceneFile(StringView path, Allocator* scratchAlloc = GLOBAL_ALLOC)
    {
        CHECK(scratchAlloc != nullptr);
        scene.reset();
        sceneFile.reset();
        sceneFile.emplace(path, MemoryMappedAccess::ReadOnly);
        scene.emplace(*sceneFile, scratchAlloc);
        LoadFSCN(*scene);
        currentSavePath = path;
    }
};

extern EditorState GEditor;

void CommitSceneToGPU(bool resetAccumulation = true);
void UpdateSceneLights();
void DeleteSelectedInstance();
void LoadScene(StringView path);
// Advances an in-flight async scene load; returns true while one is still streaming.
// Must be pumped once per editor frame.
bool PumpSceneLoad();
// Animation update, split so the per-skeleton pose evaluation can overlap the caller's per-frame
// CPU work. BeginAnimationUpdate schedules pose evaluation (non-blocking) and advances the clock;
// EndAnimationUpdate waits for it, applies rigid transforms + CPU-skins into the dynamic ring, and
// returns true if anything changed (the caller must then re-commit the scene). Call them in order,
// once per frame, with independent main-thread work in between.
void BeginAnimationUpdate(float dt);
bool EndAnimationUpdate();
// Camera animation: while an animated scene camera is active, the editor view follows it. Query
// AnimatedCameraDrivesView before BeginAnimationUpdate (it reads the scrub flag Begin then clears),
// then call ApplyAnimatedCameraToView after movement input to override the arcball for this frame.
bool AnimatedCameraDrivesView();
bool ApplyAnimatedCameraToView();
void LoadEnvMap(StringView path);
bool ApplyViewLUTSelection();
void HandleFile(const char* filePath);

void EditorDockSpaceAndMenuBar();
void FHierarchyPanel();
void FLightingPanel();
void FAnimationPanel();
void FRunningImGui();
void ClearMaterialTexturePreviewCache();

void DoRenderReadback(RendererOutputs const& outputs);
void FRendering(RendererOutputs const& outputs);