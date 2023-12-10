#include "Widgets.hpp"

using namespace EditorGlobals;
void OnImGui_EditorParamsWidget() {
	if (ImGui::Begin("Editor")) {
		ImGui::Text("Mesh Instances: %d", g_Scene.scene->storage<SceneStaticMeshComponent>().size());
		ImGui::SeparatorText("Params");
		if (ImGui::BeginTabBar("##EditorParams")) {
			if (ImGui::BeginTabItem("HDRI Probe")) {
				OnImGui_IBLProbeWidget();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Silhouette")) {
				ImGui::SliderFloat("Threshold", &g_Editor.pickerSilhouette.edgeThreshold, 0.0f, 1.0f);
				ImGui::ColorEdit3("Color", (float*)&g_Editor.pickerSilhouette.edgeColor);
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::End();
}