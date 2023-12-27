#pragma once
#include "../../../Common/Graph.hpp"
#include "SceneComponent.hpp"
#include "../../../Runtime/Asset/ResourceContainer.hpp"
#include "../../../Runtime/Asset/MeshAsset.hpp"
#include "../AssetComponet/BoneTransform.hpp"
#include "../AssetComponet/KeyshapeTransform.hpp"

struct SceneArmatureComponent : public SceneComponent {	
	static const SceneComponentType type = SceneComponentType::Armature;
	static const uint root = -1; // Not actuallly in the bone list!
	SceneArmatureComponent(Scene& scene, entt::entity ent) : SceneComponent(scene, ent, type) {};

	
	inline void setup_inv_bind_matrices(std::unordered_map<std::string, matrix> const& invBind) {
		for (auto& [s, m] : invBind) if (boneMap.contains(s)) invBindMatrices[boneMap[s]] = m;
	};
	inline void setup_local_matrices(std::unordered_map<std::string, matrix> const& local) {
		for (auto& [s, m] : local)if (boneMap.contains(s)) localMatrices[boneMap[s]] = m;
	};

	void setup(RHI::Device* device, std::unordered_map<std::string, uint> const& boneMap = {}, std::unordered_map<std::string, uint> const& keyShapeMap = {});
	void add_root_bone(const char* child);
	void add_bone_hierarchy(const char* parent, const char* child);
	void build();
	void update();

	// Name mappings
	std::unordered_map<std::string, uint> boneMap;
	std::vector<std::string> boneInvMap;
	std::unordered_map<std::string, uint> keyShapeMap;
	std::vector<std::string> keyShapeInvMap;

	std::vector<matrix> localMatrices;
	inline auto const& get_global_bone_matrices() { return globalMatrices; }
	
	inline auto const& get_child_bones(uint boneIndex) { return armature.get_graph()[boneIndex]; }
	inline const uint get_parent_bone(uint boneIndex) { return invArmature[boneIndex]; }
	inline auto const& get_root_bones() { return armature.get_graph()[root]; }

	inline uint index_bone_name(std::string const& name) { return boneMap[name]; }
	inline const entt::entity get_bone_transforms() { return boneTransforms; }
	inline const entt::entity get_keyshape_transforms() { return keyshapeTransforms; }
private:
	bool built = false;
	
	unordered_DAG<uint, uint> armature;
	std::vector<uint> invArmature;
	std::vector<uint> armatureTopsorted;

	std::vector<matrix> invBindMatrices;
	std::vector<matrix> globalMatrices;
	std::vector<matrix> tempBoneTransforms;

	entt::entity boneTransforms = entt::tombstone;
	entt::entity keyshapeTransforms = entt::tombstone;
};
