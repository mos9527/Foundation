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
	uint root = armatureTopsorted[0];
	globalMatrices[root] = localMatrices[root];
	tempBoneTransforms[root] = (invBindMatrices[root] * globalMatrices[root]).Transpose();
	for (uint i = 1; i < armatureTopsorted.size(); i++) {
		uint child = armatureTopsorted[i];
		uint parent = invArmature[child];

		globalMatrices[child] = globalMatrices[parent] * localMatrices[child];
		tempBoneTransforms[child] = (invBindMatrices[child] * globalMatrices[child]).Transpose();
	}
	final.set_bone_matrix(0, tempBoneTransforms.size(), tempBoneTransforms.data());
}