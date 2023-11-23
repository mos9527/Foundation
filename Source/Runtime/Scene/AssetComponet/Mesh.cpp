#include "../Scene.hpp"
#include "../../Asset/AssetRegistry.hpp"
#include "Mesh.hpp"
#ifdef IMGUI_ENABLED
void AssetMeshComponent::OnImGui() {	
	auto& asset = parent.get<MeshAsset>(mesh);
	ImGui::Text("Vertices: %d", asset.vertexBuffer->numVertices);
	for (uint i = 0; i < MAX_LOD_COUNT; i++) {
		ImGui::Text("LOD #%d", i);
		ImGui::Separator();
		auto* lod = asset.lodBuffers[i].get();
		ImGui::Text("Indices: %d", lod->numIndices);
		ImGui::Text("Meshlets: %d", lod->numMeshlets);
		ImGui::Text("Meshlet Tris.: %d", lod->numMeshletTriangles);
		ImGui::Text("Meshlet Vtxs.: %d", lod->numMeshletVertices);
	}
}
#endif