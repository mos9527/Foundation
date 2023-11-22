#pragma once
#include "IO.hpp"
#include "Asset.hpp"
#include "../RHI/RHI.hpp"
#include "../../Runtime/Renderer/Shaders/Shared.h"
#include "../../Common/Allocator.hpp"
#include "MeshAsset.hpp"
#include "ImageAsset.hpp"

class AssetRegistry {
	template<typename T> using Allocator = DefaultAllocator<T>;
	entt::basic_registry<entt::entity, Allocator <entt::entity>> registry;
public:	
	template<AssetRegistryDefined T> AssetHandle create() {
		static_assert(T::type != AssetType::Unknown);
		return AssetHandle{ T::type, registry.create() };
	}
	template<typename T> AssetHandle import(AssetHandle handle, T&& import) {	
		DCHECK(handle.type == Asset<T>::type);
		emplace_or_replace<Asset<T>>(handle, std::move(import));
		return handle;
	}
	template<AssetRegistryDefined T> void upload(AssetHandle handle, RHI::Device* device) {
		get<T>(handle).upload(device);
	}
	template<AssetRegistryDefined T> void upload_all(RHI::Device* device) {
		for (auto& obj : registry.storage<T>())
			obj.upload(device);
	};
	template<AssetRegistryDefined T> auto& storage() {
		return registry.storage<T>();
	}
	// returns {iterator for the next object, num of objects uploaded}
	template<AssetRegistryDefined T> std::pair<typename entt::storage<T>::iterator,uint> upload_batch(RHI::Device* device, typename entt::storage<T>::iterator begin, uint numObjs) {
		auto& storage_ = storage<T>();
		uint batched = 0;
		typename entt::storage<T>::iterator i = begin;
		for (;batched < numObjs && i != storage_.end(); i++) {
			i->upload(device);
			batched++;
		}
		return { i, batched };
	}
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
	template<typename As, typename Type> As* try_get_base_ptr(AssetHandle resource) {
		return registry.try_get<Type>(resource.entity);		
	}
	template<AssetRegistryDefined T, typename ...Args> T& emplace_or_replace(AssetHandle handle, Args &&...args) {
		return registry.emplace_or_replace<T>(handle, std::forward<decltype(args)>(args)...);
	}
};