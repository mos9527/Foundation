#pragma once
#include "AssetComponent.hpp"
#include "../../../Runtime/Asset/ResourceContainer.hpp"

struct AssetBoneTransformComponent : public AssetComponent {
	static const AssetComponentType type = AssetComponentType::Data;
	AssetBoneTransformComponent(Scene& parent, entt::entity entity) : AssetComponent(parent, entity, type) {};

	inline void setup(RHI::Device* device, std::unordered_map<std::string, uint> boneMap_){
		boneMap = boneMap_;
		boneInvMap.resize(boneMap.size());
		for (auto& [s, n] : boneMap) boneInvMap[n] = s;

		boneSpaceMatrices = std::make_unique<BufferContainer<matrix>>(device, boneMap.size(), L"Bone Matrices");
		for (uint i = 0; i < boneSpaceMatrices->NumElements(); i++) // Clear with identity transforms
			*boneSpaceMatrices->DataAt(i) = matrix::Identity;
	};
	inline auto* data() {
		CHECK(boneSpaceMatrices.get() && "Data uninitialized");
		return boneSpaceMatrices.get(); 
	}
	inline const uint get_bone_count() { return boneInvMap.size(); }
	inline auto const& get_bone_names() { return boneInvMap; }
	inline auto const& get_bone_mapping() { return boneInvMap; }
	inline uint index_bone_name(std::string const& name) { return boneMap[name]; }
	// !! COLUMN MAJOR
	inline void set_bone_matrix(uint index, matrix mat) {
		*boneSpaceMatrices->DataAt(index) = mat;
	}
	// !! COLUMN MAJOR
	inline void set_bone_matrix(uint offset, uint count, matrix* mats) {
		memcpy(boneSpaceMatrices->DataAt(offset), mats, count * sizeof(matrix));
	}
	inline void set_bone_matrix(std::string const& name, matrix mat) { set_bone_matrix(index_bone_name(name), mat); }
private:
	std::unordered_map<std::string, uint> boneMap;
	std::vector<std::string> boneInvMap;
	std::unique_ptr<BufferContainer<matrix>> boneSpaceMatrices;
};
