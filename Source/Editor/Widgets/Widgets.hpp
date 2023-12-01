#pragma once
#include "../Globals.hpp"
#include "../../../Dependencies/ImGuizmo/ImGuizmo.h"

void OnImGui_ViewportWidget();
void OnImGui_RendererParamsWidget();
void OnImGui_EditorParamsWidget();
void OnImGui_LoadingWidget();
void OnImGui_SceneGraphWidget();
void OnImGui_SceneComponentWidget();
void OnImGui_SceneComponent_TransformWidget(SceneComponent* componet);
void OnImGui_IBLProbeWidget();
