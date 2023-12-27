#pragma once
#include "SceneComponent.hpp"

struct SceneStaticMeshComponent : public SceneComponent {
	static const SceneComponentType type = SceneComponentType::StaticMesh;
	SceneStaticMeshComponent(Scene& scene, entt::entity ent) : SceneComponent(scene, ent, type) {};

	AssetHandle meshAsset;
	entt::entity materialComponent = entt::tombstone;	
};
