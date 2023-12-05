#include "SceneGraphWidgets.hpp"
#include "AssetWidgets.hpp"

using namespace EditorGlobalContext;
void OnImGui_SceneGraphWidget_SceneStaticMeshComponentWidget(SceneStaticMeshComponent* mesh) {
	ImGui::SeparatorText("Mesh");
	ImGui::Text("Name: %s", mesh->get_name());
	StaticMeshAsset& asset = mesh->parent.get<StaticMeshAsset>(mesh->meshAsset);
	if (ImGui::BeginTabBar("##SceneMesh")) {
		if (ImGui::BeginTabItem("Mesh")) {			
			ImGui::Text("Asset Name: %s", asset.GetName());
			ImGui::Text("Vertices: %d", asset.vertexBuffer->numElements);
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Material")) {
			OnImGui_AssetWidget_AssetMaterialComponent(&scene.scene->get<AssetMaterialComponent>(mesh->materialComponent));
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	mesh->update();
}

void OnImGui_SceneGraphWidget_SceneSkinnedMeshComponentWidget(SceneSkinnedMeshComponent* mesh) {
	ImGui::SeparatorText("Skinned Mesh");
	ImGui::Text("Name: %s", mesh->get_name());	
	SkinnedMeshAsset& asset = mesh->parent.get<SkinnedMeshAsset>(mesh->meshAsset);
	if (ImGui::BeginTabBar("##SceneMesh")) {
		if (ImGui::BeginTabItem("Mesh")) {			
			ImGui::Text("Asset Name: %s", asset.GetName());
			ImGui::Text("Vertices: %d", asset.vertexBuffer->numElements);
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Material")) {
			OnImGui_AssetWidget_AssetMaterialComponent(&scene.scene->get<AssetMaterialComponent>(mesh->materialComponent));
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Keyshapes")) {
			AssetKeyshapeTransformComponent& keyTransforms = scene.scene->get<AssetKeyshapeTransformComponent>(mesh->keyshapeTransformComponent);
			for (auto& [name,id] : keyTransforms.get_keyshape_mapping()) {
				ImGui::SliderFloat(name.c_str(), keyTransforms.data()->DataAt(id),0,1);
			}
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	mesh->update();
}