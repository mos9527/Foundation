#pragma once
#include <algorithm>
#include <cmath>
#include <Renderer/GPUScene.hpp>
#include <Renderer/Postprocess.hpp>
#include <Renderer/RasterEffects.hpp>
#include "Camera.hpp"
#include "Editor.hpp"
#include "Runtime/GPUScene.hpp"
#include "Runtime/Animation.hpp"
#include "Scene/Scene.hpp"

namespace Foundation::RenderCore
{
class Presenter;
}

enum class ERendererMode
{
    PathTracer = 0u,
    Raster = 1u
};

enum class ERenderFormat { HDR, SDR };

struct RenderOutputState
{
    ERenderFormat format       = ERenderFormat::HDR;
    int           targetSamples     = 0;
    int           samplePopupInput  = 4096;
    int           targetTimeSeconds = 0;
    int           timePopupInput    = 0;
    double        startTime         = 0.0;
    bool          openRenderPopup   = false;
    bool          renderPaused      = false;
    bool          renderAutoPaused  = false;
    int           autoPauseSampleLimit = 0;
    String        outputPath;
    int           previousSpp = 0;
    float         previousResolutionScale = 1.0f;
};

// Gizmo UI state (matches ImGuizmo::OPERATION and ImGuizmo::MODE)
struct GizmoState
{
    int op{7 /* TRANSLATE */}, mode{0 /* LOCAL */};
    float translateSnap{1.0f};
    float rotateSnap{15.0f};
    float scaleSnap{0.1f};
    bool showBoundingBox{true};
    bool showLightGizmos{true};
    bool showLightBVHBounds{false};
    bool showImGuizmo{true};
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
    // those tables from the FSCN scene (resident geometry handles, texture remap).
    FSceneGPUResources resources;
    Optional<FAnimationRuntime> animation;
    Optional<MemoryMappedFile> sceneFile;
    Optional<FImportedScene>     scene;
    String             currentSavePath;
    // Selection UUIDs; nil == none. Scene index == GPU index (1:1 commit order).
    FUUID              selectedInstance{};
    FUUID              selectedMaterial{};
    FUUID              selectedLight{};
    bool               scrollSelectedLightToTop = false;
    float              selectedLightHighlightStart = -1.0f;
    bool               openSelectionContextMenu = false;
    bool               applySelectionDoubleClickAction = false;
    RenderOutputState  renderTask;
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
    int matcapIndex{};
    String          matcapExternalPath;
    TextureHandle   matcapHandle{};

    RendererUBO     shaderGlobals;
    ResourceHandle  postprocessOutput;
    PostprocessUBO  postprocessGlobals;
    FArcballCamera  camera{
        .center = float3{0, 0, 0},
        .radius = 1.0f,
        .zNear = 1e-1f,
        .fovY = radians(60.f),
    };
    bool            showImGui = false;
    FEditorState    state = FEInitEnter;
    bool            cameraUpdated = true;
    float           renderResolutionScale = 1.0f; // 0.25 .. 1.0
    bool            rasterGTAO = true;
    RasterGTAOConfig rasterGTAOConfig{};

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
void RequestLoadScene(StringView path, StringView envMapPath = {});
bool PollSceneLoad();
void LoadEnvMap(StringView path);
bool ApplyViewLUTSelection();
bool ApplyMatcapSelection();
void HandleFile(const char* filePath);

void EditorDockSpaceAndMenuBar();

int SceneInstanceIndexFromId(FUUID id);
int SceneLightIndexFromId(FUUID id);
int SceneMaterialIndexFromId(FUUID id);
bool IsSelectedInstanceValid();
FSerializedBounds const* InstanceResourceBounds(FInstance const& instance);
void SelectInstance(FUUID instanceId, FUUID materialId);
void SelectLight(FUUID lightId);
void ClearSelection();
void FHierarchyPanel();
void FLightingPanel();
void FRunningImGui();
void ClearMaterialTexturePreviewCache();

void DoRenderReadback(RendererOutputs const& outputs);
void FRendering(RendererOutputs const& outputs);

void CheckDeferredSceneLoad();

// Acquire swapchain image + BeginExecute. Returns image index for EditorEndFrame.
uint32_t EditorBeginFrame(Renderer* renderer, Presenter* presenter);
// EndExecute + Present + present timing mark.
void EditorEndFrame(Renderer* renderer, Presenter* presenter);