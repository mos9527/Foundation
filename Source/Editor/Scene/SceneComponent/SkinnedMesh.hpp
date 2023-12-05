#pragma once
#include "SceneComponent.hpp"
#include "../../../Runtime/Asset/ResourceContainer.hpp"
#include "../../../Runtime/Asset/MeshAsset.hpp"

class SkinnedMeshBufferProcessor;
struct SceneSkinnedMeshComponent : public SceneComponent {
	friend SkinnedMeshBufferProcessor;
	static const SceneComponentType type = SceneComponentType::SkinnedMesh;
	SceneSkinnedMeshComponent(Scene& scene, entt::entity ent) : SceneComponent(scene, ent, type) {};
	
	AssetHandle meshAsset;
	entt::entity materialComponent = entt::tombstone;	
	entt::entity boneTransformComponent = entt::tombstone;
	entt::entity keyshapeTransformComponent = entt::tombstone;
};
