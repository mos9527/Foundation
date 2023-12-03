#include "SkinnedMesh.hpp"
#include "../Scene.hpp"
using namespace RHI;
void SceneSkinnedMeshComponent::import_skinned_mesh(Device* device, entt::entity _meshAsset) {
	auto& assetComponent = parent.get<AssetSkinnedMeshComponent>(_meshAsset);
	auto& asset = parent.get<SkinnedMeshAsset>(assetComponent.mesh);
	meshAsset = _meshAsset;
	if (asset.boneNames.size()) {
		boneLocalMatrices.resize(asset.boneNames.size());
		for (auto& mat : boneLocalMatrices) mat = matrix::Identity;
		boneGlobalMatrices.resize(asset.boneNames.size());
		for (auto& mat : boneGlobalMatrices) mat = matrix::Identity;
		boneSpaceMatrices = std::make_unique<BufferContainer<matrix>>(device, asset.boneNames.size(), L"Bone Matrices");
		auto s0 = asset.boneNames.size();
		auto s1 = boneSpaceMatrices->NumElements();
		for (uint i = 0; i < boneSpaceMatrices->NumElements(); i++) // Clear with identity transforms
			*boneSpaceMatrices->DataAt(i) = matrix::Identity; // * boneGlobalMatrix 
	}
	if (asset.keyShapeNames.size()) {
		keyshapeWeights = std::make_unique<BufferContainer<float>>(device, asset.keyShapeNames.size(), L"Keyshape Weights");
		for (uint i = 0; i < keyshapeWeights->NumElements(); i++) // Clear with 0 weights
			*keyshapeWeights->DataAt(i) = 0;
	}	
}
