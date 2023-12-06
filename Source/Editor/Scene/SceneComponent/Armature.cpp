#include "../Scene.hpp"
#include "Armature.hpp"

void SceneArmatureComponent::setup(RHI::Device* device, std::unordered_map<std::string, uint> const& boneMap_, std::unordered_map<std::string, uint> const& keyShapeMap_) {
	// Copy and build two-way mapping of bone/shape name-id
	boneMap = boneMap_;
	keyShapeMap = keyShapeMap_;

	boneInvMap.resize(boneMap.size());
	keyShapeInvMap.resize(keyShapeMap.size());
	for (auto& [s, n] : boneMap) boneInvMap[n] = s;
	for (auto& [s, n] : keyShapeMap) keyShapeInvMap[n] = s;

	// Make buffers to store the transforms	
	if (boneMap.size()) {
		boneTransforms = parent.create<AssetBoneTransformComponent>();
		auto& bt = parent.emplace<AssetBoneTransformComponent>(boneTransforms);
		bt.setup(device, boneMap);
	}
	
	if (keyShapeMap.size()) {
		keyshapeTransforms = parent.create<AssetKeyshapeTransformComponent>();
		auto& kt = parent.emplace<AssetKeyshapeTransformComponent>(keyshapeTransforms);
		kt.setup(device, keyShapeMap);
	}

	invBindMatrices.resize(boneMap.size());
	localMatrices.resize(boneMap.size());
	globalMatrices.resize(boneMap.size());
	tempBoneTransforms.resize(boneMap.size());
};
void SceneArmatureComponent::add_root_bone(const char* child) {
	CHECK(!built && "Hierarchy Already built");
	CHECK(boneMap.contains(child));
	LOG(INFO) << "Root bone " << child;
	armature.add_edge(root, boneMap[child]);
};
void SceneArmatureComponent::add_bone_hierarchy(const char* parent, const char* child) {
	CHECK(!built && "Hierarchy Already built");
	LOG(INFO) << parent << "->" << child;
	CHECK(boneMap.contains(parent) && boneMap.contains(child));
	armature.add_edge(boneMap[parent], boneMap[child]);
}

void SceneArmatureComponent::build() {	
	if (boneMap.size()) {
		// Bone hierarchy is a rooted graph.
		// A vector is sufficent.
		invArmature.resize(boneMap.size());
		for (auto& [root, children] : armature.get_graph()) {
			for (auto& child : children) {				
				invArmature[child] = root;
			}
		}
		// Use topsort to update global transforms.
		// With rooted graphs this essentially is a BFS
		armatureTopsorted = armature.topological_sort();
		CHECK(armatureTopsorted[0] == root && "Armature is not rooted. Add top-level bones to .root's hierarchy!");
		CHECK(armatureTopsorted.size() && "Bad armature...how?");
		built = true;
	}
	else {
		LOG(WARNING) << "No bones?";
	}
}

void SceneArmatureComponent::update() {
	CHECK(built && "Armature not built. Did you call build()?");	
	auto& final = parent.get<AssetBoneTransformComponent>(boneTransforms);
	for (uint i = 1; i < armatureTopsorted.size(); i++) { // 0 is the root, which does not exisit in armatures matrices
		uint child = armatureTopsorted[i];
		uint parent = invArmature[child];

		globalMatrices[child] = parent == root ? localMatrices[child] : (localMatrices[child] * globalMatrices[parent]);
		matrix bonespaceMatrix = invBindMatrices[child] * globalMatrices[child];
		tempBoneTransforms[child] = bonespaceMatrix.Transpose();
	}
	final.set_bone_matrix(0, tempBoneTransforms.size(), tempBoneTransforms.data());
}