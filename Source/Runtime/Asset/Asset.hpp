#pragma once
#include "../../pch.hpp"
enum class AssetType {
	Unknown,
	Texture,
	StaticMesh
};
class AssetRegistry;
struct AssetHandle {
	friend AssetRegistry;

	AssetType type = AssetType::Unknown;
	entt::entity entity = entt::tombstone;
	
	inline operator entt::entity() { return entity; }
	inline bool is_valid() { return entity != entt::tombstone; }
	inline void invalidate() { entity = entt::tombstone; }
};

template<typename T> struct ImportedAssetTraits {
	using imported_by = void;
	static constexpr AssetType type = AssetType::Unknown;
};
