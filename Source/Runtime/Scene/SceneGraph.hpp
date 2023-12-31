#pragma once
#include "../../Common/Graph.hpp"
#include "SceneComponent/SceneComponent.hpp"
#include "SceneComponent/Camera.hpp"
#include "SceneComponent/Light.hpp"
#include "SceneComponent/StaticMesh.hpp"
#include "SceneComponent/SkinnedMesh.hpp"
class Scene;
class SceneView;
struct SceneImporter;
/* a rooted graph representing the scene hierarchy */
class SceneGraph {
	friend Scene;
	friend SceneImporter;
	friend SceneView;

	entt::entity root;

	unordered_DAG<entt::entity,entt::entity> sceneForwardGraph;
	unordered_DAG<entt::entity,entt::entity> sceneBackwardGraph; // or sceneForwardGraph transposed

	Scene& scene;
	void update_transform(SceneComponent* parent, SceneComponent* child);
	void update_enabled(SceneComponent* src, SceneComponent* component);
	void update_version(SceneComponent* component);
	void update();
	// Depth-first-search reduction
	// calls func(entity component, entity parent) on every component encountered. Ordered from parent to children.
	void reduce(const entt::entity entity, auto&& func) {
		auto dfs = [&](auto& dfs_func, entt::entity component, entt::entity parent) -> void {
			func(component,parent);
			if (has_child(component))
				for (entt::entity child : child_of(component))
					dfs_func(dfs_func, child,component);			
		};
		dfs(dfs, entity, entity);
	}
public:
	SceneGraph(Scene& scene);
	// Getters
	const entt::entity get_root() { return root; }	
	Scene& get_scene();
	// Registry operations
	SceneComponent* get_base(const entt::entity entitiy);
	template<IsSceneComponent T> T& emplace_child_of(const entt::entity parent) {
		auto entity = get_scene().create<T>();
		add_link(parent, entity);
		T& componet = get_scene().emplace<T>(entity);		
		update();
		return componet;
	};
	void add_link(const entt::entity lhs, const entt::entity rhs) {
		sceneForwardGraph.add_edge(lhs, rhs);
		sceneBackwardGraph.add_edge(rhs, lhs);
	}
	template<IsSceneComponent T> T& emplace_at_root() {
		return emplace_child_of<T>(root);
	}
	// Updates a component and all its children's transform	
	void update_transform(const entt::entity entity);
	// Updates a component and all its children's Enable status
	void update_enabled(const entt::entity entity);
	// Updates a component and all its children's version
	void update_all_version(const entt::entity entity);
	// Updates a component itself's version
	void update(const entt::entity entity);
	// UNTESETED. perhaps testing will never be done on these things...
	//template<typename T> void remove_component(const entt::entity entity) {
	//	remove<T>(entity);
	//	sceneForwardGraph.remove_vertex(entity);
	//	sceneBackwardGraph.remove_vertex(entity);
	//}
	//void remove_link(const entt::entity lhs, const entt::entity rhs) {
	//	sceneForwardGraph.remove_edge(lhs, rhs);
	//	sceneBackwardGraph.remove_edge(rhs, lhs);
	//}
	bool contains(const entt::entity entity) {
		return sceneForwardGraph.get_graph().contains(entity) || sceneBackwardGraph.get_graph().contains(entity);
	}	
	entt::entity parent_of(const entt::entity entity) {		
		CHECK(entity != root && "Attemtping to get Root's parent");
		return *sceneBackwardGraph.get_graph().at(entity).begin();
	}
	bool has_child(const entt::entity entity) {
		return sceneForwardGraph.get_graph().contains(entity) && child_of(entity).size() > 0;
	}
	unordered_DAG<entt::entity, entt::entity>::set_type& child_of(const entt::entity entity) {
		return sceneForwardGraph.get_graph().at(entity);		
	}	
};