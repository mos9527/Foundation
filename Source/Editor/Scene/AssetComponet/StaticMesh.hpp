#pragma once
#include "AssetComponent.hpp"

struct AssetStaticMeshComponent : public AssetComponent {
	static const AssetComponentType type = AssetComponentType::StaticMesh;
	AssetStaticMeshComponent(Scene& parent, entt::entity entity) : AssetComponent(parent, entity, type) {};

	AssetHandle mesh;
};