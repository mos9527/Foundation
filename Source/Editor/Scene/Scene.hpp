#pragma once
#include "../../Runtime/Asset/AssetRegistry.hpp"
#include "SceneComponent/SceneComponent.hpp"
#include "AssetComponet/AssetComponent.hpp"
#include "SceneComponent/Camera.hpp"
#include "SceneComponent/Light.hpp"
#include "SceneComponent/StaticMesh.hpp"
#include "SceneComponent/SkinnedMesh.hpp"
#include "SceneComponent/Armature.hpp"
#include "AssetComponet/Material.hpp"
#include "AssetComponet/StaticMesh.hpp"
#include "AssetComponet/SkinnedMesh.hpp"
#include "AssetComponet/SkinnedMeshTransform.hpp"
#include "SceneGraph.hpp"
template<typename T> using Allocator = DefaultAllocator<T>;
typedef entt::basic_registry<entt::entity, Allocator <entt::entity>> SceneComponetRegistry;
typedef entt::basic_registry<entt::entity, Allocator <entt::entity>> AssetComponetRegistry;

class SceneView;
struct SceneImporter;
class Scene {
	friend SceneView;
	friend SceneGraph;
	friend SceneImporter;
	friend SceneComponent;
private:
	SceneComponetRegistry sceneComponentRegistry;
	AssetComponetRegistry assetComponentRegistry;
	AssetRegistry assetRegistry;
	/* Scene Components */
	// Create an entity in the SceneComponent registry
public:
	// Resets the Scene to a fresh state	
	void reset() {
		sceneComponentRegistry.clear();
		assetComponentRegistry.clear();
		assetRegistry.clear();
		graph.reset(new SceneGraph(*this));
	}
	template<IsSceneComponent T> T& get(entt::entity entity) {
		return sceneComponentRegistry.get<T>(entity);
	}
	template<IsSceneComponent T> T* try_get(entt::entity entity) {
		return sceneComponentRegistry.try_get<T>(entity);
	}
	template<IsSceneComponent T> auto& storage() {
		return sceneComponentRegistry.storage<T>();
	}
	template<IsSceneComponent T> size_t index(entt::entity entity) {
		return storage<T>().index(entity);
	}
	template<IsSceneComponent T> entt::entity create() {
		return sceneComponentRegistry.create();
	}
	template<IsSceneComponent T> void remove(entt::entity entity) {
		sceneComponentRegistry.remove<T>(entity);
	}
	template<IsSceneComponent T> void sort(auto&& comp) {
		return sceneComponentRegistry.sort<T>(comp);
	}
	template<IsSceneComponent T> T& emplace(entt::entity entity) {
		sceneComponentRegistry.emplace<SceneComponentType>(entity, T::type);
		return sceneComponentRegistry.emplace<T>(entity, *this, entity);
	}
	/* Asset Components */
	// Create an entity in the AssetComponent registry
	template<IsAssetComponent T> T& get(entt::entity entity) {
		return assetComponentRegistry.get<T>(entity);
	}
	template<IsAssetComponent T> T* try_get(entt::entity entity) {
		return assetComponentRegistry.try_get<T>(entity);
	}
	template<IsAssetComponent T> entt::entity create() {
		return assetComponentRegistry.create();
	}
	template<IsAssetComponent T> void remove(entt::entity entity) {
		assetComponentRegistry.remove<T>(entity);
	}
	template<IsAssetComponent T> auto& storage() {
		return assetComponentRegistry.storage<T>();
	}
	template<IsAssetComponent T> T& emplace(entt::entity entity) {
		assetComponentRegistry.emplace<AssetComponentType>(entity, T::type);
		return assetComponentRegistry.emplace<T>(entity, *this, entity);
	}
	/* Assets */
	// Create an AssetHandle in the asset registry.	
	template<Asset T> T& get(AssetHandle resource) {
		return assetRegistry.get<T>(resource);
	}
	template<Asset T> T* try_get(AssetHandle resource) {
		return assetRegistry.try_get<T>(resource);
	}
	template<Asset T> void clean() {
		assetRegistry.clean<T>();
	}
	template<Asset T> AssetHandle create() {
		return assetRegistry.create<T>();
	}
	template<Asset T> void remove(AssetHandle resource) {
		assetRegistry.remove<T>(resource);
	}
	// Copies the `import` data to the Upload heap. Which is still CPU memory.
	// Assets needs Uploading. Call member function Upload(UploadContext*)
	// on a UploadContext for queueing the upload on the GPU.
	// Importing, and registry operations overall,
	// are NOT thread-safe.
	// Use a mutex if you're loading multi-threaded
	template<Importable T> auto& import_asset(AssetHandle handle, RHI::Device* device, T* import) {
		return assetRegistry.import(handle, device, import);
	}
public:
	/* Helpers */
	// Retrives whether the entity's has a valid Type, or exists at all
	// T : Either SceneComponentType, or AssetComponentType
	// returns false for invalid type / non-existing entity. true otherwise.
	template<typename T> bool valid(entt::entity entity) {
		if constexpr (std::is_same_v<T, SceneComponentType>) {
			return sceneComponentRegistry.valid(entity) && get_type<SceneComponentType>(entity) != SceneComponentType::Unknown;
		}
		else if constexpr (std::is_same_v<T, AssetComponentType>) {
			return assetComponentRegistry.valid(entity) && get_type<AssetComponentType>(entity) != AssetComponentType::Unknown;
		}
	}
	// Retrives the type of the SceneComponet or AssetComponent bound to entity
	// T : Either SceneComponentType, or AssetComponentType
	// uhh also see https://stackoverflow.com/questions/38304847/constexpr-if-and-static-assert
	// triggering a substution failure seemed like a better fit...
	template<typename T> T get_type(entt::entity entity) {
		if constexpr (std::is_same_v<T, SceneComponentType>) {
			return sceneComponentRegistry.get<SceneComponentType>(entity);
		}
		else if constexpr (std::is_same_v<T, AssetComponentType>) {
			return assetComponentRegistry.get<AssetComponentType>(entity);
		}
	}
	// Retrives the T* of the object bound to entity
	// T : Either SceneComponent, or AssetComponent	
	template<typename T> T* get_base(const entt::entity entity) {
		if constexpr (std::is_same_v<SceneComponent, T>) {			
			switch (get_type<SceneComponentType>(entity))
			{
			case SceneComponentType::Collection:
				return &get<SceneCollectionComponent>(entity);
			case SceneComponentType::Camera:
				return &get<SceneCameraComponent>(entity);
			case SceneComponentType::StaticMesh:
				return &get<SceneStaticMeshComponent>(entity);
			case SceneComponentType::SkinnedMesh:
				return &get<SceneSkinnedMeshComponent>(entity);
			case SceneComponentType::Light:
				return &get<SceneLightComponent>(entity);
			default:
				return nullptr;
			}
		}
		else if constexpr (std::is_same_v<AssetComponent, T>) {
			switch (get_type<AssetComponentType>(entity))
			{
			case AssetComponentType::StaticMesh:
				return &get<AssetStaticMeshComponent>(entity);
			case AssetComponentType::SkinnedMesh:
				return &get<AssetSkinnedMeshComponent>(entity);
			case AssetComponentType::Material:
				return &get<AssetMaterialComponent>(entity);
			default:
				return nullptr;
			}
		}
		else {
			return nullptr;
		}
	}
	Scene() {
		graph = std::make_unique<SceneGraph>(*this);
	};
	Scene(const Scene&) = delete;
	Scene(Scene&&) = default;
private:
	size_t version = 0;
public:
	const size_t get_version() { return version; }
	std::unique_ptr<SceneGraph> graph;
};
