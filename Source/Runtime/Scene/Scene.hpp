#pragma once
#include "SceneGraph.hpp"
#include "AssetComponet/Material.hpp"
#include "AssetComponet/Mesh.hpp"
typedef entt::registry SceneComponetRegistry;
typedef entt::registry AssetComponetRegistry;
class Scene {
	friend SceneGraph;
private:
	SceneComponetRegistry sceneComponentRegistry;
	AssetComponetRegistry assetComponentRegistry;
	AssetRegistry assetRegistry;
public:
	/* Scene Components */
	template<IsSceneComponent T> entt::entity create() {
		return sceneComponentRegistry.create();
	}
	template<IsSceneComponent T> void remove(entt::entity entity) {
		sceneComponentRegistry.remove<T>(entity);
	}
	template<IsSceneComponent T> T& get(entt::entity entity) {
		return sceneComponentRegistry.get<T>(entity);
	}
	template<IsSceneComponent T> T* try_get(entt::entity entity) {
		return sceneComponentRegistry.try_get<T>(entity);
	}
	template<IsSceneComponent T> T& emplace_or_replace(entt::entity entity, auto&& args...) {
		sceneComponentRegistry.emplace_or_replace<SceneComponentType>(entity, T::type);
		return sceneComponentRegistry.emplace_or_replace<T>(entity, std::forward<decltype(args)>(args)...);
	}
	/* Asset Components */
	template<IsAssetComponent T> entt::entity create() {
		return assetComponentRegistry.create();
	}
	template<IsAssetComponent T> void remove(entt::entity entity) {
		assetComponentRegistry.remove<T>(entity);
	}
	template<IsAssetComponent T> T& get(entt::entity entity) {
		return assetComponentRegistry.get<T>(entity);
	}
	template<IsAssetComponent T> T* try_get(entt::entity entity) {
		return assetComponentRegistry.try_get<T>(entity);
	}
	template<IsAssetComponent T> T& emplace_or_replace(entt::entity entity, auto&& args...) {
		assetComponentRegistry.emplace_or_replace<AssetComponentType>(entity, T::type);
		return assetComponentRegistry.emplace_or_replace(entity, std::forward<decltype(args)>(args)...);
	}
	/* Assets */
	// Create an AssetHandle in the asset registry.	
	template<Asset T> AssetHandle create() {
		return assetRegistry.create();
	}
	template<Asset T> void remove(AssetHandle resource) {
		assetRegistry.remove(resource);
	}
	template<Asset T> T& get(AssetHandle resource) {
		return assetRegistry.get<T>(resource);
	}
	template<Asset T> T* try_get(AssetHandle resource) {
		return assetRegistry.try_get<T>(resource);
	}
	// Copies the `import` data to the Upload heap. Which is still CPU memory
	// Assets needs Uploading. Use upload<T> on a UploadContext for that.
	template<Importable T> T& import_asset(AssetHandle handle, RHI::Device* device, T* import) {
		return assetRegistry.import(handle, device, import);
	}
	/* Helpers */	
	// Retrives the T* of the object bound to entity
	// T : Either SceneComponent, or AssetComponent
	template<typename T> T* try_get_base(const entt::entity entity) {
		if constexpr (std::is_same_v<SceneComponent, T>) {
			SceneComponentType type = sceneComponentRegistry.get<SceneComponentType>(entity);
			switch (type)
			{
			case SceneComponentType::Collection:
				return try_get<SceneCollectionComponent>(entity);
			case SceneComponentType::Camera:
				return try_get<SceneCameraComponent>(entity);
			case SceneComponentType::Mesh:
				return try_get<SceneMeshComponent>(entity);
			case SceneComponentType::Light:
				return try_get<SceneLightComponent>(entity);
			default:
				return nullptr;
			}
		}
		else if constexpr (std::is_same_v<AssetComponent, T>) {
			AssetComponentType type = assetComponentRegistry.get<AssetComponentType>(entity);
			switch (type)
			{
			case AssetComponentType::Mesh:
				return try_get<MeshAssetComponent>(entity);
			case AssetComponentType::Material:
				return try_get<MaterialAssetComponet>(entity);
			default:
				return nullptr;
			}
		}
		else {
			static_assert(false && "Invalid cast detected. Only SceneComponent & AssetComponent are supported.");
		}
	}
};
