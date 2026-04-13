#pragma once
#include "Editor.hpp"
#include "Scene/Texture.hpp"
#include <ImGuizmo.h>

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
        vec3 up      = vec3(0, 1, 0);
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

/* -- Grouped editor state structs -- */

// Scene-lifetime data: created in ReplaceScene, read everywhere, reset together
struct EditorDocument
{
    Vector<GSInstance> instances{GLOBAL_ALLOC};
    Vector<GSMaterial> materials{GLOBAL_ALLOC};
    Vector<GSMesh>     meshes{GLOBAL_ALLOC};
    Vector<uint32_t>   blases{GLOBAL_ALLOC};
    FScene             scene{GLOBAL_ALLOC};
    String             currentSavePath;
    int                selectedInstance = -1;
    int                selectedMaterial = -1;
    int                selectedLight    = -1;
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
    ImGuizmo::MODE      mode = ImGuizmo::WORLD;
};

/* -- Extern globals (defined in EditorState.cpp) -- */
extern FEditorState    FEState;
extern UBO             GShaderGlobals;
extern FArcballCamera  GCamera;
extern bool            cameraUpdated;
extern bool            GShowImGui;

extern EditorDocument  GDoc;
extern RenderWorkflow  GRenderImageTask;
extern GizmoState      GGizmo;
extern RendererConfig  GRendererConfig;
extern ERendererMode   GRendererMode;

/* -- Cross-file editor functions -- */
void UpdateSceneLights();
void ReplaceScene(StringView path);
void SaveScene(StringView path);
void LoadEnvMap(StringView path);
void HandleFile(const char* filePath);

void EditorDockSpaceAndMenuBar();
void FHierarchyPanel();
void FLightingPanel();
void DrawLightGizmos();
void FRunningImGui();
bool ImHDRColorEdit(const char* label, float4& value, float maxScale = 100.0f);

void DoHDRReadback(PTReadbackHandles const& handles);
void DoSDRReadback(PTReadbackHandles const& handles);
void FRendering(PTReadbackHandles const& handles);
