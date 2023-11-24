#pragma once
#include "../../Common/Graph.hpp"
#include "SceneComponent/SceneComponent.hpp"
#include "SceneComponent/Camera.hpp"
#include "SceneComponent/Light.hpp"
#include "SceneComponent/Mesh.hpp"
class Scene;
class SceneView;
struct SceneImporter;
/* a rooted graph representing the scene hierarchy */
class SceneGraph {
	friend struct SceneImporter;
	friend class SceneView;
	entt::entity root;

	DAG<entt::entity> sceneForwardGraph;
	DAG<entt::entity> sceneBackwardGraph; // or sceneForwardGraph transposed

	Scene& scene;
	void add_link(const entt::entity lhs, const entt::entity rhs) {
		sceneForwardGraph.add_edge(lhs, rhs);
		sceneBackwardGraph.add_edge(rhs, lhs);
	}
	// xxx this needs to be a memebr function because of friend declartions...
	void update_parent_child(SceneComponent* parent, SceneComponent* child);
	// Depth-first-search reduction
	// calls func(entity component, entity parent) on every component encountered. Ordered from parent to children.
	void reduce(const entt::entity entity, auto&& func) {
		auto dfs = [&](auto& dfs_func, entt::entity component, entt::entity parent) -> void {
			if (component == parent && component != root) // look for the actual parent with the first iteration
				parent = parent_of(component); // xxx possible circular dependency detection? though impossible to create with SceneGraph apis alone.
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
		update(entity, true); // ensure the component's associative states are correct upon creation
		return componet;
	};
	// Updates a selected component, and therefore scene.
	// [associative]
	// When true, updates associative properties for this subtree of component
	// These properites are:
	// * Transformation
	// Finally, their `version` are also updated.
	void update(const entt::entity entity, bool associative = false);
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
	DAG<entt::entity>::tree_type& child_of(const entt::entity entity) {
		return sceneForwardGraph.get_graph().at(entity);		
	}
#ifdef IMGUI_ENABLED
	entt::entity ImGui_selected_entity = entt::tombstone;
	void OnImGuiEntityWindow(entt::entity entity);
	void OnImGui();
#endif
};