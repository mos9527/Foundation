#include "SceneGraphWidgets.hpp"
#include "AssetWidgets.hpp"

using namespace EditorGlobals;
void OnImGui_SceneGraphWidget_SceneCameraComponentWidget(SceneCameraComponent* camera) {
	ImGui::SeparatorText("Camera");
	ImGui::SliderAngle("FOV", &camera->fov, 0.01f, XM_PIDIV2);
	ImGui::Checkbox("Orthographic", &camera->orthographic);
	ImGui::SliderFloat("Min Luminance", &camera->logLuminanceMin,-20,20);
	ImGui::SliderFloat("Luminance Range", &camera->logLuminanceRange,0,20);
	ImGui::SliderFloat("Luminance Adapt Rate", &camera->luminanceAdaptRate, 0, 5);
	ImGui::SliderFloat("Z Near", &camera->nearZ, 0, 1000);
	ImGui::SliderFloat("Z Far", &camera->farZ, 0, 1000);
	camera->update();
}