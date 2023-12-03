#pragma once
#include "SceneComponent.hpp"
#include "../../../Runtime/Asset/ResourceContainer.hpp"
#include "../../../Runtime/Asset/MeshAsset.hpp"
#include "../AssetComponet/SkinnedMeshTransform.hpp"
struct SceneArmatureComponent : public SceneComponent {
	static const SceneComponentType type = SceneComponentType::Armature;
	SceneArmatureComponent(Scene& scene, entt::entity ent) : SceneComponent(scene, ent, type) {};

	struct ArmatureBone {

		matrix localTransform;
	};
	
	void setup(RHI::Device* device, SkinnedMeshAsset& asset);
	void add_bone_hierarchy(const char* parent, const char* child);
	void set_root_bone(const char* name);
	void set_bone_local_transform(const char* name, matrix transform);

	// Name mappings
	std::vector<std::string> boneNames;
	std::unordered_map<std::string, uint> invBoneNames;
	std::vector<std::string> keyShapeNames;
	std::unordered_map<std::string, uint> invKeyShapeNames;

	std::vector<matrix> localMatrices;	

	std::vector<std::vector<uint>> armature;	
public:
	inline const entt::entity get_transform_asset() { return transformAsset; }
private:
	uint rootBone = -1;
	entt::entity transformAsset = entt::tombstone;
	// Bind transforms
	std::vector<matrix> invBindMatrices;
	std::vector<matrix> globalMatrices;
};
