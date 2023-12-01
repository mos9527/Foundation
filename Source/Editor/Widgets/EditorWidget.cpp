#include "Widgets.hpp"

using namespace EditorGlobalContext;
void OnImGui_EditorParamsWidget() {
	if (ImGui::Begin("Editor")) {
		ImGui::Text("Mesh Instances: %d", scene.scene->storage<SceneMeshComponent>().size());
		ImGui::SeparatorText("Params");
		if (ImGui::BeginTabBar("##EditorParams")) {
			if (ImGui::BeginTabItem("HDRI Probe")) {
				OnImGui_IBLProbeWidget();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Silhouette")) {
				ImGui::SliderFloat("Threshold", &editor.silhouetteParam.edgeThreshold, 0.0f, 1.0f);
				ImGui::ColorEdit3("Color", (float*)&editor.silhouetteParam.edgeColor);
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::End();
}