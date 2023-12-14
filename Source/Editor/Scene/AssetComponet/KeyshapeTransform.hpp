#pragma once
#include "AssetComponent.hpp"
#include "../../../Runtime/Asset/ResourceContainer.hpp"

struct AssetKeyshapeTransformComponent : public AssetComponent {
	static const AssetComponentType type = AssetComponentType::Data;
	AssetKeyshapeTransformComponent(Scene& parent, entt::entity entity) : AssetComponent(parent, entity, type) {};

	inline void setup(RHI::Device* device, std::unordered_map<std::string, uint> keyshapeIDMapping) {
		keyshapeMap = keyshapeIDMapping;
		keyshapeInvMap.resize(keyshapeMap.size());
		for (auto& [name, id] : keyshapeIDMapping)
			keyshapeInvMap[id] = name;	

		keyshapeWeights_srv.first = std::make_unique<BufferContainer<float>>(device, keyshapeIDMapping.size(), L"Keyshape Weights");
		for (uint i = 0; i < keyshapeWeights_srv.first->NumElements(); i++) // Clear with 0 weights
			*keyshapeWeights_srv.first->DataAt(i) = .0f;
	};
	inline auto& data() {
		return keyshapeWeights_srv;
	}
	inline const uint get_keyshape_count() { return keyshapeInvMap.size(); }
	inline auto const& get_keyshape_names() { return  keyshapeInvMap; }
	inline auto const& get_keyshape_mapping() { return keyshapeMap; }
	inline uint index_keyshape_name(std::string const& name) { return keyshapeMap[name]; }
	inline void set_keyshape_weight(uint index, float weight) {
		*keyshapeWeights_srv.first->DataAt(index) = weight;
	}
	inline void set_keyshape_weight(uint offset, uint count, float* weights) {
		memcpy(keyshapeWeights_srv.first->DataAt(offset), weights, count * sizeof(float));
	}
	inline void set_keyshape_weight(std::string const& name, float weight) {		
		set_keyshape_weight(index_keyshape_name(name), weight);
	}
	inline float get_keyshape_weight(uint index) { return *keyshapeWeights_srv.first->DataAt(index); }
	inline float get_keyshape_weight(std::string const& name) { return get_keyshape_weight(index_keyshape_name(name)); }
private:
	std::unordered_map<std::string, uint> keyshapeMap;
	std::vector<std::string> keyshapeInvMap;
	std::pair<std::unique_ptr<BufferContainer<float>>, std::unique_ptr<RHI::ShaderResourceView>> keyshapeWeights_srv;
};