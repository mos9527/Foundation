#pragma once

enum class AssetType {
	Unknown,
	Texture,
	StaticMesh,
	SkinnedMesh
};
class AssetRegistry;
// -> RHI Asset
struct AssetHandle {
	friend AssetRegistry;

	AssetType type = AssetType::Unknown;
	entt::entity entity = entt::tombstone;
	
	inline operator entt::entity() { return entity; }
};

template<typename T> struct ImportedAssetTraits {
	using imported_by = void;
	static constexpr AssetType type = AssetType::Unknown;
};
