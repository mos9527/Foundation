#pragma once
#include "../Component.hpp"
#include "../../AssetRegistry/IO.hpp"
#include "../../AssetRegistry/AssetRegistry.hpp"

struct MaterialComponet : public SceneComponent {
	float4 albedo;
	float4 pbr;
	float4 emissive;

	AssetHandle albedoImage;
	AssetHandle normalMapImage;
	AssetHandle pbrMapImage;
	AssetHandle emissiveMapImage;
};

template<> struct SceneComponentTraits<MaterialComponet> {
	static constexpr SceneComponentType type = SceneComponentType::Material;
};