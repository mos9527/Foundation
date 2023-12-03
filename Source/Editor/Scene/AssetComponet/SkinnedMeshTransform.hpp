#pragma once
#include "AssetComponent.hpp"
#include "../../../Runtime/Asset/ResourceContainer.hpp"
#include "../../../Runtime/Asset/MeshAsset.hpp"

struct AssetSkinnedMeshTransformComponent : public AssetComponent {
	static const AssetComponentType type = AssetComponentType::Data;
	AssetSkinnedMeshTransformComponent(Scene& parent, entt::entity entity) : AssetComponent(parent, entity, type) {};

	void setup(RHI::Device* device, uint boneCount = 0, uint keyshapeCount = 0);

	std::unique_ptr<BufferContainer<matrix>> boneSpaceMatrices;
	std::unique_ptr<BufferContainer<float>> keyshapeWeights;

	const bool valid() { return boneSpaceMatrices.get() && keyshapeWeights.get(); }
	const uint get_bone_count() { return boneCount; }
	const uint get_keyshape_count() { return keyshapeCount; }
private:
	uint boneCount = -1;
	uint keyshapeCount = -1;
};
