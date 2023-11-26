#include "SceneComponent.hpp"
#include "../Scene.hpp"
void SceneComponent::update(bool associative) {
	parent.graph->update(entity, associative);
}
void SceneComponent::set_local_transform(AffineTransform T) {
	localTransform = T;
	update(true);
}
