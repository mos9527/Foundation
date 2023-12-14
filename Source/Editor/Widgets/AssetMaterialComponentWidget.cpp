#include "AssetWidgets.hpp"
using namespace EditorGlobals;

void DrawOneImage(TextureAsset* asset) {
	ImVec2 size{ 128,128 };
	ImGui::Image((ImTextureID)asset->textureSRV->allocate_online_descriptor().get_gpu_handle().ptr, size);
}

void OnImGui_AssetWidget_AssetMaterialComponent(AssetMaterialComponent* material) {
	bool edited = false;
	ImGui::Text("Material Name: %s", material->get_name());
	ImGui::Checkbox("Alpha Mapped", &material->alphaMapped);	
	if (ImGui::IsItemEdited()) edited |= true;
	auto* pAlbedo = g_Scene.scene->try_get<TextureAsset>(material->albedoImage);
	auto* pPBR = g_Scene.scene->try_get<TextureAsset>(material->pbrMapImage);
	auto* pEmissive = g_Scene.scene->try_get<TextureAsset>(material->emissiveMapImage);
	auto* pNormal = g_Scene.scene->try_get<TextureAsset>(material->normalMapImage);
	if (ImGui::BeginTabBar("##Materials")) {
		if (ImGui::BeginTabItem("Albedo")) {
			if (!pAlbedo)
				ImGui::ColorEdit4("Albedo", (float*)&material->albedo);
			else
				DrawOneImage(pAlbedo);
			if (ImGui::IsItemEdited()) edited |= true;
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("AO/Rough/Metal")) {
			if (!pPBR)
				ImGui::SliderFloat3("PBR", (float*)&material->pbr, 0.0f, 1.0f);
			else
				DrawOneImage(pPBR);
			if (ImGui::IsItemEdited()) edited |= true;
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Emissive")) {
			if (!pEmissive)
				ImGui::ColorEdit4("Emissive", (float*)&material->emissive);
			else
				DrawOneImage(pEmissive);
			if (ImGui::IsItemEdited()) edited |= true;
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Normal")) {
			if (!pNormal)
				ImGui::Text("[not texture mapped]");
			else
				DrawOneImage(pNormal);
			if (ImGui::IsItemEdited()) edited |= true;
			ImGui::EndTabItem();
		}
		if (edited) {
			material->update();
			g_Scene.scene->graph->update_all_version(g_Scene.scene->graph->get_root());
		}
		ImGui::EndTabBar();
	}
}