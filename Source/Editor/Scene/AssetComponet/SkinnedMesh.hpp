#pragma once
#include "AssetComponent.hpp"

struct AssetSkinnedMeshComponent : public AssetComponent {
	static const AssetComponentType type = AssetComponentType::SkinnedMesh;
	AssetSkinnedMeshComponent(Scene& parent, entt::entity entity) : AssetComponent(parent, entity, type) {};

	AssetHandle mesh;
};
