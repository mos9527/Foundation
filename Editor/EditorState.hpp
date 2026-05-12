#pragma once
#include "Editor.hpp"
#include "Render/ViewLUTs.hpp"
#include "Scene/Scene.hpp"
#include "Scene/Texture.hpp"
#include <ImGuizmo.h>
#include <algorithm>
#include <cmath>

struct FArcballCamera
{
    static constexpr char kControlsText[] = "Mouse Left: Rotate | Mouse Right: Pan | Mouse Wheel: Zoom | WASD: Move | Shift: Fast";

    float3 center, position;
    float radius;
    quat rot;
    float zNear, fovY, aspect;
    mat4 view, proj;
    float moveSpeed = 2.0f;
    // WASD key state
    bool keyW = false, keyA = false, keyS = false, keyD = false;
    bool keyShift = false;
    // Called every frame: continuously move center based on WASD state
    bool UpdateMovement(float dt)
    {
        vec3 moveDir(0.0f);
        vec3 forward = rot * vec3(0, 0, -1); // camera forward (note: view direction is -Z)
        vec3 right   = rot * vec3(1, 0, 0);
        if (keyW) moveDir += forward;
        if (keyS) moveDir -= forward;
        if (keyA) moveDir -= right;
        if (keyD) moveDir += right;
        if (glm::dot(moveDir, moveDir) > 1e-6f)
        {
            moveDir = glm::normalize(moveDir);
            float speed = moveSpeed * (keyShift ? 4.0f : 1.0f);
            center += moveDir * speed * dt;
            return true;
        }
        return false;
    }

    bool Update(SDL_Event const& event)
    {
        bool updated = false;
        if (event.type == SDL_EVENT_MOUSE_MOTION)
        {
            if (event.motion.state & SDL_BUTTON_LMASK)
            {
                float yawDelta = -event.motion.xrel * 1e-2f;
                float pitchDelta = -event.motion.yrel * 1e-2f;
                quat yawRot = angleAxis(yawDelta, vec3(0, 1, 0));
                quat pitchRot = angleAxis(pitchDelta, vec3(1, 0, 0));
                rot = normalize(yawRot * rot * pitchRot);
                updated = true;
            }
            if (event.motion.state & SDL_BUTTON_RMASK)
            {
                vec3 right = rot * vec3(1, 0, 0);
                vec3 up = rot * vec3(0, 1, 0);
                center -= right * (event.motion.xrel * radius * 1e-3f);
                center += up * (event.motion.yrel * radius * 1e-3f);
                updated = true;
            }
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL)
        {
            radius -= event.wheel.y * radius * 1e-1f;
            radius = radius < 1e-3f ? 1e-3f : radius;
            updated = true;
        }
        // ---
        proj = infinitePerspectiveRHReverseZ(fovY, aspect, zNear);
        vec3 dir = rot * vec3(0, 0, 1);
        position = center + radius * dir;
        view = viewMatrixRHReverseZ(position, rot);
        return updated;
    }
};

enum class ERendererMode
{
    PathTracer,
    Raster
};

enum class ERenderFormat { HDR, SDR };

// Scene-lifetime data: created in ReplaceScene, read everywhere, reset together
struct EditorDocument
{
    Vector<GSInstance> instances{GLOBAL_ALLOC};
    Vector<GSMaterial> materials{GLOBAL_ALLOC};
    Vector<GSMesh>     meshes{GLOBAL_ALLOC};
    Vector<uint32_t>   blases{GLOBAL_ALLOC};
    Vector<GSCurveSet> curves{GLOBAL_ALLOC};
    Vector<uint32_t>   curveBlases{GLOBAL_ALLOC};
    Vector<GSLight>    lights{GLOBAL_ALLOC};
    Optional<FileReader> sceneReader;
    Optional<FScene>     scene;
    String             currentSavePath;
    int                selectedInstance = -1;
    int                selectedMaterial = -1;
    int                selectedLight    = -1;

    [[nodiscard]] bool HasScene() const { return scene.has_value(); }
    FScene& Scene()
    {
        CHECK(scene.has_value());
        return *scene;
    }
    FScene const& Scene() const
    {
        CHECK(scene.has_value());
        return *scene;
    }
    void OpenSceneReader(StringView path)
    {
        scene.reset();
        sceneReader.reset();
        sceneReader.emplace(path);
        scene.emplace(*sceneReader);
        currentSavePath = path;
    }
};

// Offline render workflow state: flows from file dialog → popup → readback
struct RenderWorkflow
{
    ERenderFormat format       = ERenderFormat::HDR;
    int           targetSamples     = 0;
    int           samplePopupInput  = 4096;
    bool          openRenderPopup   = false;
    bool          renderPaused      = false;
    String        outputPath;
};

// Gizmo UI state
struct GizmoState
{
    ImGuizmo::OPERATION op   = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE      mode = ImGuizmo::LOCAL;
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
    EditorDocument  doc;
    RenderWorkflow  renderTask;
    GizmoState      gizmo;
    CameraApertureState aperture;
    EditorViewportState viewport;
    RendererConfig  rendererConfig;
    ERendererMode   rendererMode = ERendererMode::PathTracer;
    int             viewLUTSdrIndex = kDefaultViewLUTSdr;
    int             viewLUTHdrIndex = kDefaultViewLUTHdr;
    String          viewLUTSdrExternalPath;
    String          viewLUTHdrExternalPath;

    UBO             shaderGlobals;
    FArcballCamera  camera{
        .center = float3{0, 0, 0},
        .radius = 1.0f,
        .zNear = 0.1f,
        .fovY = radians(60.f),
    };
    bool            showImGui = false;
    FEditorState    state = FEInitEnter;
    bool            cameraUpdated = true;
};

extern EditorState GEditor;

void CommitSceneToGPU(bool resetAccumulation = true);
void UpdateSceneLights();
void ReplaceScene(StringView path);
void LoadEnvMap(StringView path);
bool ApplyViewLUTSelection();
void HandleFile(const char* filePath);

void EditorDockSpaceAndMenuBar();
void FHierarchyPanel();
void FLightingPanel();
void FRunningImGui();

void DoRenderReadback(RendererHandles const& handles);
void FRendering(RendererHandles const& handles);