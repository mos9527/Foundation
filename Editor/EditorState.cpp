#include "EditorState.hpp"

EditorDocument  GDoc;
RenderWorkflow  GRenderImageTask;
GizmoState      GGizmo;
RendererConfig  GRendererConfig;
ERendererMode   GRendererMode = ERendererMode::PathTracer;

UBO GShaderGlobals;

FArcballCamera GCamera{
    .center = float3{0, 0, 0},
    .radius = 1.0f,
    .zNear = 0.1f,
    .fovY = radians(60.f),
};
bool GShowImGui = false;
FEditorState FEState = FEInitEnter;
bool cameraUpdated = true;
