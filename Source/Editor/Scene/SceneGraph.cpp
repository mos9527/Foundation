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
void SceneGraph::update_transform(SceneComponent* parent, SceneComponent* child) {
	if (parent == child && parent->entity != root) {
		parent = get_base(parent_of(parent->entity));
	}
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
	SceneComponent* thisComponent = get_base(entity);
	if (associative)
		reduce(entity, [&](entt::entity current, entt::entity parent) -> void {
			SceneComponent* parentComponent = get_base(parent);
			SceneComponent* childComponent = get_base(current);
			update_transform(parentComponent, childComponent);
			parentComponent->enabled = thisComponent->enabled;
			childComponent->enabled = thisComponent->enabled;
		});
}
SceneComponent* SceneGraph::get_base(const entt::entity entity) {
	return scene.get_base<SceneComponent>(entity);	
}
