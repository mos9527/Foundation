#pragma once
#include "../../Common/Graph.hpp"
#include "SceneComponent/SceneComponent.hpp"
#include "SceneComponent/Camera.hpp"
#include "SceneComponent/Light.hpp"
#include "SceneComponent/Mesh.hpp"

class Scene;
class SceneGraphView;
/* a rooted graph representing the scene hierarchy */
class SceneGraph {
	friend class SceneGraphView;
	entt::entity root;

	DAG<entt::entity> sceneForwardGraph;
	DAG<entt::entity> sceneBackwardGraph; // or sceneForwardGraph transposed

	Scene& scene;
	size_t version = 0;
	inline void update_global_transform(SceneComponent* child, SceneComponent* parent) {
		child->globalTransform = parent->globalTransform * child->localTransform;
	}
public:
	SceneGraph(Scene& scene);
	// Getters
	const entt::entity get_root() { return root; }
	inline Scene& get_scene() { return scene; }
	// Registry operations
	SceneComponent* try_get_base(const entt::entity entitiy);
	template<IsSceneComponent T> entt::entity create_child_of(const entt::entity parent);
	// Depth-first-search reducion
	// calls func(entity component, entity parent) on every component encountered. Ordered from parent to children.
	void reduce(const entt::entity entitiy, auto&& func) {
		auto dfs = [&](auto& dfs_func, entt::entity component, entt::entity parent) -> void {
			if (component == parent) // look for the actual parent with the first iteration			
				parent = parent_of(component); // xxx possible circular dependency detection? though impossible to create with SceneGraph apis alone.
			func(component,parent);
			for (entt::entity child : child_of(component))
				dfs_func(dfs_func, child,component);
		};
		dfs(dfs, entity, entity);
	}
	// Updates associative properties for this subtree of component
	// These properites are:
	// * Transformation
	void update(const entt::entity entity) { 
		version++;
		reduce(entity, [&](entt::entity current, entt::entity parent) -> void {
			SceneComponent* currentComponet = try_get_base(current);
			SceneComponent* parentComponent = try_get_base(parent);
			update_global_transform(currentComponet, parentComponent);
		});
	}
	template<typename T> void remove_component(const entt::entity entity) {
		remove<T>(entity);
		sceneForwardGraph.remove_vertex(entity);
		sceneBackwardGraph.remove_vertex(entity);
	}
	void remove_link(const entt::entity lhs, const entt::entity rhs) {
		sceneForwardGraph.remove_edge(lhs, rhs);
		sceneBackwardGraph.remove_edge(rhs, lhs);
	}
	void add_link(const entt::entity lhs, const entt::entity rhs) {
		sceneForwardGraph.add_edge(lhs, rhs);
		sceneBackwardGraph.add_edge(rhs, lhs);
	}
	bool contains(const entt::entity entity) {
		return sceneForwardGraph.get_graph().contains(entity);
	}
	entt::entity parent_of(const entt::entity entity) {
		return *sceneBackwardGraph.get_graph().at(entity).begin();
	}
	DAG<entt::entity>::tree_type& child_of(const entt::entity entity) {
		return sceneBackwardGraph.get_graph().at(entity);		
	}
#ifdef IMGUI_ENABLED
	void OnImGui();
#endif
};