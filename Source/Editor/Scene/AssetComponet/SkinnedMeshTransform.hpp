#pragma once
#include "AssetComponent.hpp"
#include "../../../Runtime/Asset/ResourceContainer.hpp"
#include "../../../Runtime/Asset/MeshAsset.hpp"

struct SkinnedMeshTransform : public AssetComponent {
	static const AssetComponentType type = AssetComponentType::Data;
	SkinnedMeshTransform(Scene& parent, entt::entity entity) : AssetComponent(parent, entity, type) {};

	void setup_with_asset(RHI::Device* device, SkinnedMeshAsset& asset);

	std::vector<matrix> boneLocalMatrices;
	std::vector<matrix> boneGlobalMatrices;
	std::unique_ptr<BufferContainer<matrix>> boneSpaceMatrices;
	std::unique_ptr<BufferContainer<float>> keyshapeWeights;
};
