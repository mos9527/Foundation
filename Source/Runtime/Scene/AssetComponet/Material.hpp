#pragma once
#include "AssetComponent.hpp"

struct AssetMaterialComponent : public AssetComponent {	
	static const AssetComponentType type = AssetComponentType::Material;
	AssetMaterialComponent(Scene& parent, entt::entity entity) : AssetComponent(parent, entity, type) {};

	bool alphaMapped = false; // assumes albedo alpha channel packs alpha map

	float4 albedo;
	float4 pbr;
	float4 emissive;

	AssetHandle albedoImage;
	AssetHandle normalMapImage;
	AssetHandle pbrMapImage;
	AssetHandle emissiveMapImage;
	bool has_alpha() {
		return alphaMapped;
	}
};