#include "SceneGraphWidgets.hpp"
#include "AssetWidgets.hpp"

using namespace EditorGlobalContext;
void OnImGui_SceneGraphWidget_SceneStaticMeshComponentWidget(SceneStaticMeshComponent* mesh) {
	ImGui::SeparatorText("Mesh");
	ImGui::Text("Name: %s", mesh->get_name());
	ImGui::SliderInt("LOD Override", &mesh->lodOverride, -1, MAX_LOD_COUNT);
	ImGui::Checkbox("Occludee", &mesh->isOccludee);
	if (ImGui::BeginTabBar("##SceneMesh")) {
		if (ImGui::BeginTabItem("Mesh")) {
			OnImGui_AssetWidget_AssetStaticMeshComponent(&scene.scene->get<AssetStaticMeshComponent>(mesh->meshAsset));
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Material")) {
			OnImGui_AssetWidget_AssetMaterialComponent(&scene.scene->get<AssetMaterialComponent>(mesh->materialAsset));
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	mesh->update();
}

void OnImGui_SceneGraphWidget_SceneSkinnedMeshComponentWidget(SceneSkinnedMeshComponent* mesh) {
	ImGui::SeparatorText("Skinned Mesh");
	ImGui::Text("Name: %s", mesh->get_name());
	AssetSkinnedMeshComponent& assetComponent = mesh->parent.get<AssetSkinnedMeshComponent>(mesh->meshAsset);
	SkinnedMeshAsset& asset = mesh->parent.get<SkinnedMeshAsset>(assetComponent.mesh);
	if (ImGui::BeginTabBar("##SceneMesh")) {
		if (ImGui::BeginTabItem("Mesh")) {
			OnImGui_AssetWidget_AssetSkinnedMeshComponent(&scene.scene->get<AssetSkinnedMeshComponent>(mesh->meshAsset));
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Material")) {
			OnImGui_AssetWidget_AssetMaterialComponent(&scene.scene->get<AssetMaterialComponent>(mesh->materialAsset));
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	mesh->update();
}