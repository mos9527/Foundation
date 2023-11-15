#pragma once
#include "../../pch.hpp"
#include "MeshAsset.hpp"
namespace IO {		
	class AssetManager;
	struct asset_handle {
		friend AssetManager;
		enum asset_type {
			Unknown,
			Texture,
			StaticMesh
		};

		asset_type get_resource_type() { return type; }
		template<typename T> static constexpr asset_type get_resource_type() { return Unknown; }
		template<> static constexpr asset_type get_resource_type<void>() { return Texture; } // xxx
		template<> static constexpr asset_type get_resource_type<StaticMeshAsset>() { return StaticMesh; }
		asset_handle() {};
		asset_handle(entt::entity resource, asset_type type) : resource(resource), type(type) {};	
	private:		
		asset_type type = Unknown;
		entt::entity resource = entt::tombstone;
	};
}