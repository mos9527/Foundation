#pragma once
#include "SceneComponent.hpp"
#include "../../../Runtime/Asset/ResourceContainer.hpp"
#include "../../../Runtime/Asset/MeshAsset.hpp"

class SkinnedMeshBufferProcessor;
struct SceneSkinnedMeshComponent : public SceneComponent {
	friend SkinnedMeshBufferProcessor;
	static const SceneComponentType type = SceneComponentType::SkinnedMesh;
	SceneSkinnedMeshComponent(Scene& scene, entt::entity ent) : SceneComponent(scene, ent, type) {};

	entt::entity materialAsset = entt::tombstone;

	std::vector<matrix> boneLocalMatrices;
	std::unique_ptr<BufferContainer<float>> keyshapeWeights;
	
	void import_skinned_mesh(RHI::Device* device, entt::entity asset);	
	entt::entity get_mesh_asset() { return meshAsset; }

	int lodOverride = -1;
private:	
	std::vector<matrix> boneGlobalMatrices;
	std::unique_ptr<BufferContainer<matrix>> boneSpaceMatrices;
	entt::entity meshAsset = entt::tombstone;	
};
