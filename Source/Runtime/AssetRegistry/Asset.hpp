#pragma once
#include "../RHI/RHI.hpp"
enum class AssetType {
	Unknown,
	Image,
	StaticMesh
};
class AssetRegistry;
struct AssetHandle {
	friend AssetRegistry;
	AssetType type = AssetType::Unknown;
	entt::entity entity = entt::tombstone;
	inline operator entt::entity() { return entity; }
};
template<typename Imports> struct Asset {
	using imported_type = Imports;
	static constexpr AssetType type = AssetType::Unknown;
	entt::entity entity;
	std::string name;

	Asset(Imports&&) {};
	void upload(RHI::Device* device) {};
};

/* task concepts */
template<typename T> concept AssetRegistryDefined = requires(T t,typename T::imported_type&& imported, RHI::Device * device) {
	T(std::move(imported));
	t.upload(device);	
};