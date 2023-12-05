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
	LOG(INFO) << boneNames.size();
	localMatrices.resize(boneNames.size());
	globalMatrices.resize(boneNames.size());
	invArmature.resize(boneNames.size());
};
void SceneArmatureComponent::add_bone_hierarchy(const char* parent, const char* child) {
	CHECK(armatureTopsorted.size() == 0 && "Hierarchy Already built");
	LOG(INFO) << parent << "->" << child;
	// CHECK(invBoneNames.contains(parent) && invBoneNames.contains(child));
	armature.add_edge(invBoneNames[parent], invBoneNames[child]);	
}
void SceneArmatureComponent::set_root_bone(const char* name) {
	CHECK(armatureTopsorted.size() == 0 && "Hierarchy Already built");
	rootBone = invBoneNames[name];
}
void SceneArmatureComponent::set_bone_local_transform(const char* name, matrix transform) {
	localMatrices[invBoneNames[name]] = transform;
}
void SceneArmatureComponent::update() {
	// HACK: Lazy eval by checking topsorted size	
	if (armatureTopsorted.size() == 0) {		
		armatureTopsorted = armature.topological_sort();
		for (auto& [parent,children] :armature.get_graph()) {
			for (auto& child : children) {
				invArmature[child] = parent;
			}
		}
	}
	CHECK(armatureTopsorted.front() == rootBone && "Invalid bone hierarchy");
}