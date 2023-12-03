#pragma once
#include "SceneComponent.hpp"

struct SceneStaticMeshComponent : public SceneComponent {
	static const SceneComponentType type = SceneComponentType::StaticMesh;
	SceneStaticMeshComponent(Scene& scene, entt::entity ent) : SceneComponent(scene, ent, type) {};

	entt::entity meshAsset = entt::tombstone;
	entt::entity materialAsset = entt::tombstone;

	int lodOverride = -1;
	bool isOccludee = true; // can be occluded by other geometry
};
