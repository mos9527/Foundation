#pragma once
#include "AssetComponent.hpp"

struct AssetMaterialComponent : public AssetComponent {	
	static const AssetComponentType type = AssetComponentType::Material;
	AssetMaterialComponent(Scene& parent, entt::entity entity) : AssetComponent(parent, entity, type) {};

	float4 albedo;
	float4 pbr;
	float4 emissive;

	AssetHandle albedoImage;
	AssetHandle normalMapImage;
	AssetHandle pbrMapImage;
	AssetHandle emissiveMapImage;
#ifdef IMGUI_ENABLED
	virtual void OnImGui();
#endif
};