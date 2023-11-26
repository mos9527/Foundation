#include "SceneGraph.hpp"
#include "Scene.hpp"
SceneGraph::SceneGraph(Scene& scene) : scene(scene) {
	root = scene.create<SceneComponent>();
	auto& root_collection = scene.emplace<SceneCollectionComponent>(root);
	root_collection.set_name("Scene Root");
};
Scene& SceneGraph::get_scene() {
	return scene;
}
void SceneGraph::update_parent_child(SceneComponent* parent, SceneComponent* child) {
	if (parent && child) {
		child->version = get_scene().version;
		parent->version = get_scene().version;
		child->globalTransform = parent->globalTransform * child->localTransform;
	}
	else if (child) {
		child->version = get_scene().version;
		child->globalTransform = child->localTransform;
	}
}
void SceneGraph::update(const entt::entity entity, bool associative) {
	scene.version++;	
	get_base(entity)->version = scene.version;
	if (associative)
		reduce(entity, [&](entt::entity current, entt::entity parent) -> void {
			SceneComponent* parentComponent = get_base(parent);
			SceneComponent* childComponent = get_base(current);
			update_parent_child(parentComponent, childComponent);
		});
}
SceneComponent* SceneGraph::get_base(const entt::entity entity) {
	return scene.get_base<SceneComponent>(entity);	
}
