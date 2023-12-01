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
	child->version = get_scene().version;
	if (parent != child) {
		child->globalTransform = parent->globalTransform * child->localTransform;
	}
	else {
		child->globalTransform = child->localTransform;
	}
}
void SceneGraph::update_transform(const entt::entity entity) {		
	update();
	reduce(entity, [&](entt::entity current, entt::entity parent) -> void {
		if (current == parent && current != root) {
			parent = parent_of(current);
		}
		SceneComponent* parentComponent = get_base(parent);
		SceneComponent* childComponent = get_base(current);
		update_transform(parentComponent, childComponent);		
	});
}
void SceneGraph::update_enabled(SceneComponent* src, SceneComponent* component) {
	component->version = scene.version;
	src->version = scene.version;
	component->enabled = src->enabled;
}
void SceneGraph::update_enabled(const entt::entity entity) {
	update();
	SceneComponent* thisComponent = get_base(entity);
	reduce(entity, [&](entt::entity current, entt::entity parent) -> void {
		SceneComponent* childComponent = get_base(current);
		update_enabled(thisComponent, childComponent);
	});
}
void SceneGraph::update() {
	scene.version++;
}
void SceneGraph::update_all() {
	update();
	update_transform(root);
	update_enabled(root);
}
void SceneGraph::update(const entt::entity entity) {
	SceneComponent* thisComponent = get_base(entity);
	update();
	thisComponent->version = scene.version;
}
SceneComponent* SceneGraph::get_base(const entt::entity entity) {
	return scene.get_base<SceneComponent>(entity);	
}
