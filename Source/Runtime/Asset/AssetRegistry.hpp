#pragma once
#include "Asset.hpp"
#include "../RHI/RHI.hpp"
#include "../../Runtime/Renderer/Shaders/Shared.h"
#include "../../Common/Allocator.hpp"

#include "UploadContext.hpp"
#include "TextureAsset.hpp"
#include "MeshAsset.hpp"
template<typename T> concept Importable = !std::is_void_v<typename ImportedAssetTraits<T>::imported_by>;
template<typename T> concept Asset = requires (T a, RHI::Device * device, UploadContext * ctx) {
	T::type; // Has a type member
	T(device, nullptr); // Also takes a resource pointer
	a.Upload(ctx); // Can be uploaded via UploadContext
	a.Clean(); // Can be cleaned
};
class AssetRegistry {
	template<typename T> using Allocator = DefaultAllocator<T>;
	entt::basic_registry<entt::entity, Allocator <entt::entity>> registry;
public:	
	template<Asset T> AssetHandle create() {		
		return AssetHandle{ T::type, registry.create() };
	}
	template<Importable T> auto& import(AssetHandle handle, RHI::Device* device, T* import) {	
		DCHECK(handle.type == ImportedAssetTraits<T>::type && "Importing asset onto a incompatible handle");
		return emplace<typename ImportedAssetTraits<T>::imported_by>(handle, device, import);		
	}
	template<Asset T> void upload(AssetHandle handle, UploadContext* ctx) {
		get<T>(handle).Upload(ctx);
	}
	template<Asset T> void upload_all(UploadContext* ctx) {
		for (auto& obj : registry.storage<T>())
			obj.Upload(ctx);
	};
	template<Asset T> auto& storage() {
		return registry.storage<T>();
	}
	template<Asset T> void clean() {
		for (auto& obj : registry.storage<T>())
			obj.Clean();
	}
	template<Asset T> T& get(AssetHandle resource) {
		return registry.get<T>(resource.entity);
	}
	template<Asset T> T* try_get(AssetHandle resource) {
		return registry.try_get<T>(resource.entity);
	}
	template<Asset T> void remove(AssetHandle resource) {
		registry.remove<T>(resource);
	}
private:
	template<Asset T, typename ...Args> T& emplace(AssetHandle handle, Args &&...args) {
		return registry.emplace<T>(handle, std::forward<decltype(args)>(args)...);
	}
};
