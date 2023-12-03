#include "SkinnedMeshTransform.hpp"
using namespace RHI;
void SkinnedMeshTransform::setup_with_asset(RHI::Device* device, SkinnedMeshAsset& asset) {
	if (asset.boneNames.size()) {
		boneLocalMatrices.resize(asset.boneNames.size());
		for (auto& mat : boneLocalMatrices) mat = matrix::Identity;
		boneGlobalMatrices.resize(asset.boneNames.size());
		for (auto& mat : boneGlobalMatrices) mat = matrix::Identity;
		boneSpaceMatrices = std::make_unique<BufferContainer<matrix>>(device, asset.boneNames.size(), L"Bone Matrices");
		for (uint i = 0; i < boneSpaceMatrices->NumElements(); i++) // Clear with identity transforms
			*boneSpaceMatrices->DataAt(i) = matrix::Identity; // * boneGlobalMatrix 
	}
	if (asset.keyShapeNames.size()) {
		keyshapeWeights = std::make_unique<BufferContainer<float>>(device, asset.keyShapeNames.size(), L"Keyshape Weights");
		for (uint i = 0; i < keyshapeWeights->NumElements(); i++) // Clear with 0 weights
			*keyshapeWeights->DataAt(i) = 0;
	}
}
