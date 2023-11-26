#include "AssetWidgets.hpp"
using namespace EditorGlobalContext;
void OnImGui_AssetWidget_AssetMeshComponent(AssetMeshComponent* mesh) {
	ImGui::Text("Mesh Name: %s", mesh->get_name());	
	auto& asset = scene.scene->get<MeshAsset>(mesh->mesh);
	ImGui::Text("Vertices: %d", asset.vertexBuffer->numVertices);
	static int selectedLod = 0;
	ImGui::SliderInt("LOD", &selectedLod, 0, MAX_LOD_COUNT);
	ImGui::Text("LOD #%d", selectedLod);
	ImGui::Separator();
	auto* lod = asset.lodBuffers[selectedLod].get();
	ImGui::Text("Indices: %d", lod->numIndices);
	ImGui::Text("Meshlets: %d", lod->numMeshlets);
	ImGui::Text("Meshlet Tris.: %d", lod->numMeshletTriangles);
	ImGui::Text("Meshlet Vtxs.: %d", lod->numMeshletVertices);	
}