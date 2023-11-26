#pragma once
#include "AssetComponent.hpp"

struct AssetMeshComponent : public AssetComponent {
	static const AssetComponentType type = AssetComponentType::Mesh;
	AssetMeshComponent(Scene& parent, entt::entity entity) : AssetComponent(parent, entity, type) {};

	AssetHandle mesh;
};