#include "SceneComponent.hpp"
#include "../Scene.hpp"
void SceneComponent::update() {
	parent.graph->update(entity);
}
void SceneComponent::set_enabled(bool enabled_) {
	if (enabled != enabled_) {
		enabled = enabled_;
		parent.graph->update_enabled(entity);
	}
}
void SceneComponent::set_selected(bool selected_) {
	if (selected_ != selected){
		selected = selected_;
		update();
	}
}
void SceneComponent::set_local_transform(AffineTransform T) {
	localTransform = T;
	parent.graph->update_transform(entity);
}
