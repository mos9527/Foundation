#pragma once
#include "IO.hpp"
#include "../RHI/RHI.hpp"
#include "MeshAsset.hpp"
#include "MeshImporter.hpp"

#include "../../Runtime/Renderer/Shaders/Shared.h"
template<typename T> concept Cleanable = requires(T obj) { obj.clean(); };
class AssetRegistry;
struct asset_handle {
	friend AssetRegistry;
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

class AssetRegistry {
	entt::registry registry;
public:
	template<typename T, typename Src> asset_handle load(Src& src) {};
	template<> asset_handle load<StaticMeshAsset>(mesh_static& mesh);

	template<typename T> void upload(RHI::Device* device, T* resource) {};
	template<> void upload<StaticMeshAsset>(RHI::Device* device, StaticMeshAsset* resource);

	template<typename T> void upload_all(RHI::Device* device) {
		for (auto& obj : registry.storage<T>())
			upload<T>(device, &obj);
	};

	template<Cleanable T> void clean() {
		for (auto& obj : registry.storage<T>())
			obj.clean();
	}
	template<typename T> T& get(asset_handle resource) {
		return registry.get<T>(resource.resource);
	}

	template<typename T> T* try_get(asset_handle resource) {
		return registry.try_get<T>(resource.resource);
	}
};