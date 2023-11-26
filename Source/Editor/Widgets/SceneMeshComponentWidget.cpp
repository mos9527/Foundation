#include "SceneGraphWidgets.hpp"
#include "AssetWidgets.hpp"

using namespace EditorGlobalContext;
void OnImGui_SceneGraphWidget_SceneMeshComponent(SceneMeshComponent* mesh) {	
	ImGui::Text("Name: %s", mesh->get_name());
	ImGui::SliderInt("LOD Override", &mesh->lodOverride, -1, MAX_LOD_COUNT);
	ImGui::Checkbox("Occludee", &mesh->isOccludee);
	if (ImGui::BeginTabBar("##SceneMesh")) {
		if (ImGui::BeginTabItem("Mesh")) {
			OnImGui_AssetWidget_AssetMeshComponent(&scene.scene->get<AssetMeshComponent>(mesh->meshAsset));
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Material")) {
			OnImGui_AssetWidget_AssetMaterialComponent(&scene.scene->get<AssetMaterialComponent>(mesh->materialAsset));
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}	
}