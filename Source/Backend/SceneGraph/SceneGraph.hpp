#pragma once
#include "../../Common/Graph.hpp"
#include "../AssetRegistry/AssetRegistry.hpp"
#include "../RHI/RHI.hpp"
#include "Component.hpp"
#include "Components/StaticMesh.hpp"
#include "Components/Camera.hpp"
struct aiScene;
enum class SceneComponentTag {
	Root,
	Collection,
	Camera,
	StaticMesh
};
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
		registry.emplace<SceneComponentTag>(root, SceneComponentTag::Root);
	};
	template<typename T> T& get(entt::entity entity) {
		return registry.get<T>(entity);
	}
	template<typename T> T* try_get(entt::entity entity) {
		return registry.try_get<T>(entity);
	}
	template<typename T> entt::entity emplace(entt::entity parent, auto&& args...) {
		auto entity = registry.create();
		registry.emplace<T>(entity, std::forward<decltype(args)>(args)...);
		add_link(parent, entity);
		return entity;
	}
	SceneComponent* try_get_base_ptr(entt::entity entity) {
		SceneComponentTag type = get<SceneComponentTag>(entity);
		switch (type)
		{
		case SceneComponentTag::Collection:
			return try_get<ComponentCollection>(entity);
		case SceneComponentTag::Camera:
			return try_get<CameraComponent>(entity);
		case SceneComponentTag::StaticMesh:
			return try_get<StaticMeshComponent>(entity);
		default:
		case SceneComponentTag::Root:
			return nullptr;			
		}
	}
	template<typename T> void remove_node(entt::entity entity) {
		registry.remove<T>(entity);
		DAG::remove_vertex(entity);
	}
	void remove_link(entt::entity lhs, entt::entity rhs) {
		DAG::remove_edge(lhs, rhs);
	}
	void add_link(entt::entity lhs, entt::entity rhs) {
		DAG::add_edge(lhs, rhs);
	}
	using DAG<entt::entity>::get_graph;
	void load_from_aiScene(const aiScene* scene);
	void log_entites();	
};