#pragma once
#include "IO.hpp"
#include "Asset.hpp"
#include "../RHI/RHI.hpp"
#include "../../Runtime/Renderer/Shaders/Shared.h"

class AssetRegistry {
	entt::registry registry;
public:
	template<typename T> AssetHandle import(T& import) {
		auto handle = create(Asset<T>::type);
		emplace_or_replace<Asset<T>>(handle, import);
		return handle;
	}
	template<AssetRegistryDefined T> void upload(AssetHandle handle, RHI::Device* device) {
		get<T>(handle).upload(device);
	}
	template<AssetRegistryDefined T> void upload_all(RHI::Device* device) {
		for (auto& obj : registry.storage<T>())
			obj.upload(device);
	};
	template<AssetRegistryDefined T> void clean() {
		for (auto& obj : registry.storage<T>())
			obj.clean();
	}
	template<AssetRegistryDefined T> T& get(AssetHandle resource) {
		return registry.get<T>(resource.entity);
	}

	template<AssetRegistryDefined T> T* try_get(AssetHandle resource) {
		return registry.try_get<T>(resource.entity);
	}
	template<AssetRegistryDefined T, typename ...Args> T& emplace_or_replace(AssetHandle handle, Args &&...args) {
		return registry.emplace_or_replace<T>(handle, std::forward<decltype(args)>(args)...);
	}
	AssetHandle create(AssetType type) {
		return AssetHandle{ type, registry.create() };
	}
};