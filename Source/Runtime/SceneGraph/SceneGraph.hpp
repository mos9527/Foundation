#pragma once
#include "../../Common/Graph.hpp"
#include "../AssetRegistry/AssetRegistry.hpp"
#include "../RHI/RHI.hpp"
#include "Component.hpp"
#include "Components/StaticMesh.hpp"
#include "Components/Camera.hpp"
#include "Components/Material.hpp"
#include "Components/Light.hpp"

struct aiScene;
class AssetRegistry;
class SceneGraphView;
/* a rooted graph representing the scene hierarchy */
typedef size_t scene_version;
class SceneGraph : DAG<entt::entity> {
	friend class SceneGraphView;
	using DAG::graph;
	entt::entity root;
	entt::registry registry;

	entt::entity active_camera = entt::tombstone;

	AssetRegistry& assets;

	scene_version version = 0;
	void update(entt::entity entity, entt::entity parent);
public:
	SceneGraph(AssetRegistry& assets) : assets(assets) {
		root = registry.create();
		auto& root_collection = emplace<CollectionComponent>(root);
		root_collection.set_name("Scene Root");
	};
	// updates componets' Global Transform and (xxx) their hierarchical bounding boxes
	void update() {
		version++;
		update(root, root); 
	}
	size_t get_version() { return version; }
	template<SceneComponentDefined T> T& get(const entt::entity entity) {
		return registry.get<T>(entity);
	}
	template<SceneComponentDefined T> T* try_get(const entt::entity entity) {
		return registry.try_get<T>(entity);
	}
	template<SceneComponentDefined T> auto& emplace(const entt::entity entity) {
		auto type = SceneComponentTraits<T>::type;
		CHECK(type != SceneComponentType::Unknown && "Unsupported type.");
		registry.emplace<SceneComponentType>(entity, type);		
		return registry.emplace<T>(entity, entity);
	}	
	template<SceneComponentDefined T> entt::entity create_child_of(const entt::entity parent) {
		auto entity = registry.create();
		emplace<T>(entity);
		add_link(parent, entity);
		return entity;
	}
	const entt::entity get_root() { return root; }
	SceneComponent* try_get_base_ptr(const entt::entity entity) {
		SceneComponentType type = get<SceneComponentType>(entity);
		switch (type)
		{
		case SceneComponentType::Collection:
			return try_get<CollectionComponent>(entity);
		case SceneComponentType::Camera:
			return try_get<CameraComponent>(entity);
		case SceneComponentType::StaticMesh:
			return try_get<StaticMeshComponent>(entity);
		case SceneComponentType::Light:
			return try_get<LightComponent>(entity);
		case SceneComponentType::Material:
			return try_get<MaterialComponet>(entity);		
		default:
			return nullptr;			
		}
	}
	void set_active_camera(entt::entity entity){
		CHECK(try_get<CameraComponent>(entity) != nullptr && "Not a camera");
		active_camera = entity;
	}
	CameraComponent& get_active_camera() {
		return get<CameraComponent>(active_camera);
	}
	template<typename T> void remove_node(const entt::entity entity) {
		registry.remove<T>(entity);
		DAG::remove_vertex(entity);
	}
	void remove_link(const entt::entity lhs, const entt::entity rhs) {
		DAG::remove_edge(lhs, rhs);
	}
	void add_link(const entt::entity lhs, const entt::entity rhs) {
		CHECK(rhs != root && "Attempting to create a circular depndency (Connecting to Root)");
		DAG::add_edge(lhs, rhs);
	}
	void load_from_aiScene(const aiScene* scene, path_t scenePath);	
#ifdef IMGUI_ENABLED
	void OnImGui();
#endif
};