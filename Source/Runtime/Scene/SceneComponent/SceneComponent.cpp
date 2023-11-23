#include "SceneComponent.hpp"
#include "../Scene.hpp"

void SceneComponent::set_local_transform(AffineTransform T) {
	localTransform = T;
	parent.graph.update(entity);
}
