#pragma once
#include "../../Common/Graph.hpp"
#include "../AssetRegistry/AssetRegistry.hpp"
#include "../RHI/RHI.hpp"
#include "Component.hpp"
#include "Components/StaticMesh.hpp"
#include "Components/Camera.hpp"
#include "Components/Material.hpp"

struct aiScene;
class AssetRegistry;
class SceneGraphView;
/* a rooted graph representing the scene hierarchy */
class SceneGraph : DAG<entt::entity> {
	friend class SceneGraphView;
	using DAG::graph;
	entt::entity root;
	entt::registry registry;

	entt::entity active_camera = entt::tombstone;

	AssetRegistry& assets;
public:
	SceneGraph(AssetRegistry& assets) : assets(assets) {
		root = registry.create();
		registry.emplace<SceneComponentType>(root, SceneComponentType::Root);
	};
	template<typename T> T& get(const entt::entity entity) {
		return registry.get<T>(entity);
	}
	template<typename T> T* try_get(const entt::entity entity) {
		return registry.try_get<T>(entity);
	}
	template<typename T, typename... Args> void emplace(const entt::entity entity, Args&&... args) {
		auto type = SceneComponentTraits<T>::type;
		CHECK(type != SceneComponentType::Unknown && "Unsupported type.");
		registry.emplace<SceneComponentType>(entity, type);		
		registry.emplace<T>(entity, std::forward<decltype(args)>(args)...);
	}	
	template<typename T, typename... Args> entt::entity create_child_of(const entt::entity parent, Args&&... args) {
		auto entity = registry.create();
		emplace<T>(entity, std::forward<decltype(args)>(args)...);
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
		default:
		case SceneComponentType::Root:
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
		DAG::add_edge(lhs, rhs);
	}
	void load_from_aiScene(const aiScene* scene);
	void log_entites();		
};