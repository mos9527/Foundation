#pragma once
#include "../Component.hpp"
#include "../../AssetRegistry/IO.hpp"
#include "../../AssetRegistry/AssetRegistry.hpp"

struct MaterialComponet : public SceneComponent {
	using SceneComponent::SceneComponent;
	float4 albedo;
	float4 pbr;
	float4 emissive;

	AssetHandle albedoImage;
	AssetHandle normalMapImage;
	AssetHandle pbrMapImage;
	AssetHandle emissiveMapImage;
#ifdef IMGUI_ENABLED
	virtual void OnImGui() {
		if (!albedoImage.is_valid())
			ImGui::ColorEdit4("Albedo", (float*)&albedo);
		else
			ImGui::Text("Albedo : Texture Mapped");
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
};

template<> struct SceneComponentTraits<MaterialComponet> {
	static constexpr SceneComponentType type = SceneComponentType::Material;
};