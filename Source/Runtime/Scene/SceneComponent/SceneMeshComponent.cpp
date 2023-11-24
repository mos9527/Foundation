#include "Mesh.hpp"
#include "../../Asset/AssetRegistry.hpp"
#include "../AssetComponet/Mesh.hpp"
#include "../AssetComponet/Material.hpp"
#include "../Scene.hpp"

#ifdef IMGUI_ENABLED
void SceneMeshComponent::OnImGui() {
	ImGui::SliderInt("LOD Override", &lodOverride, -1, MAX_LOD_COUNT);
	ImGui::Checkbox("Occludee", &isOccludee);
	ImGui::Checkbox("Visible", &visible);
	ImGui::Checkbox("Draw Bounding Box", &drawBoundingBox);
	auto* mesh = parent.try_get<AssetMeshComponent>(meshAsset);
	if (mesh)
		mesh->OnImGui();
	auto* material = parent.try_get<AssetMaterialComponent>(materialAsset);
	if (material)
		material->OnImGui();
}
#endif // IMGUI_ENABLED
