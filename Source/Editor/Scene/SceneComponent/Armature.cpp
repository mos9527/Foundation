#include "../Scene.hpp"
#include "Armature.hpp"

void SceneArmatureComponent::setup(RHI::Device* device,SkinnedMeshAsset& asset) {
	boneNames = asset.boneNames;
	invBoneNames = asset.invBoneNames;
	keyShapeNames = asset.keyShapeNames;
	invKeyShapeNames = asset.invKeyShapeNames;

	invBindMatrices = asset.invBindMatrices;

	transformAsset = parent.create<AssetSkinnedMeshTransformComponent>();
	auto& transform = parent.emplace<AssetSkinnedMeshTransformComponent>(transformAsset);
	transform.setup(device, boneNames.size(), keyShapeNames.size());

	armature.resize(boneNames.size());
	localMatrices.resize(boneNames.size());
};
void SceneArmatureComponent::add_bone_hierarchy(const char* parent, const char* child) {
	armature[invBoneNames[parent]].push_back(invBoneNames[child]);
}
void SceneArmatureComponent::set_root_bone(const char* name) {
	rootBone = invBoneNames[name];
}
void SceneArmatureComponent::set_bone_local_transform(const char* name, matrix transform) {
	localMatrices[invBoneNames[name]] = transform;
}