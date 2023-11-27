#include "SceneComponent.hpp"
#include "../Scene.hpp"
void SceneComponent::update() {
	parent.graph->update(entity);
}
void SceneComponent::set_enabled(bool enabled_) {
	enabled = enabled_;
	parent.graph->update_enabled(entity);
}
void SceneComponent::set_local_transform(AffineTransform T) {
	localTransform = T;
	parent.graph->update_transform(entity);
}
