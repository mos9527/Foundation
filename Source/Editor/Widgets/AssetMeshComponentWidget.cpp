#include "AssetWidgets.hpp"
using namespace EditorGlobalContext;
void OnImGui_AssetWidget_AssetStaticMeshComponent(AssetStaticMeshComponent* mesh) {
	ImGui::Text("Mesh Name: %s", mesh->get_name());
	auto& asset = scene.scene->get<StaticMeshAsset>(mesh->mesh);
	ImGui::Text("Vertices: %d", asset.vertexBuffer->numElements);
	static int selectedLod = 0;
	ImGui::SliderInt("LOD", &selectedLod, 0, MAX_LOD_COUNT);
	ImGui::Text("LOD #%d", selectedLod);
	ImGui::Separator();
	auto* lod = asset.lodBuffers[selectedLod].get();
	ImGui::Text("Indices: %d", lod->numIndices);
#ifdef RHI_USE_MESH_SHADER
	ImGui::Text("Meshlets: %d", lod->numMeshlets);
	ImGui::Text("Meshlet Tris.: %d", lod->numMeshletTriangles);
	ImGui::Text("Meshlet Vtxs.: %d", lod->numMeshletVertices);
#endif
}

void OnImGui_AssetWidget_AssetSkinnedMeshComponent(AssetSkinnedMeshComponent* mesh) {
	ImGui::Text("Skinned Mesh Name: %s", mesh->get_name());
	auto& asset = scene.scene->get<SkinnedMeshAsset>(mesh->mesh);
	ImGui::Text("Vertices: %d", asset.vertexBuffer->numElements);
	static int selectedLod = 0;
	ImGui::SliderInt("LOD", &selectedLod, 0, MAX_LOD_COUNT);
	ImGui::Text("LOD #%d", selectedLod);
	ImGui::Separator();
	auto* lod = asset.lodBuffers[selectedLod].get();
	ImGui::Text("Indices: %d", lod->numIndices);
	// xxx there's more!
#ifdef RHI_USE_MESH_SHADER
	ImGui::Text("Meshlets: %d", lod->numMeshlets);
	ImGui::Text("Meshlet Tris.: %d", lod->numMeshletTriangles);
	ImGui::Text("Meshlet Vtxs.: %d", lod->numMeshletVertices);
#endif
}