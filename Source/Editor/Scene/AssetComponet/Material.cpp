#include "Material.hpp"
#ifdef IMGUI_ENABLED
void AssetMaterialComponent::OnImGui() {
	ImGui::Checkbox("Alpha Mapped", &alphaMapped);
	ImGui::Text("Alpha Mode: %d", has_alpha());
	if (!albedoImage.is_valid())
		ImGui::ColorEdit4("Albedo", (float*)&albedo);
	else
		ImGui::Text("Albedo : Texture Mapped"); // xxx info for these? do it right away...
	if (!pbrMapImage.is_valid())
		ImGui::SliderFloat3("AO/Rough/Metal", (float*)&pbr, 0.0f, 1.0f);
	else
		ImGui::Text("PBR : Texture Mapped");
	if (!emissiveMapImage.is_valid())
		ImGui::ColorEdit4("Emissive", (float*)&emissive);
	else
		ImGui::Text("Emissive : Texture Mapped");
}
#endif