#include "SkinnedMeshTransform.hpp"
using namespace RHI;
void AssetSkinnedMeshTransformComponent::setup(RHI::Device* device, uint boneCount_, uint keyshapeCount_) {
	boneCount = boneCount_;
	keyshapeCount = keyshapeCount_;
	if (boneCount) {		
		boneSpaceMatrices = std::make_unique<BufferContainer<matrix>>(device, boneCount, L"Bone Matrices");
		for (uint i = 0; i < boneSpaceMatrices->NumElements(); i++) // Clear with identity transforms
			*boneSpaceMatrices->DataAt(i) = matrix::Identity;
	}
	if (keyshapeCount) {
		keyshapeWeights = std::make_unique<BufferContainer<float>>(device, keyshapeCount, L"Keyshape Weights");
		for (uint i = 0; i < keyshapeWeights->NumElements(); i++) // Clear with 0 weights
			*keyshapeWeights->DataAt(i) = 0;
	}
}
