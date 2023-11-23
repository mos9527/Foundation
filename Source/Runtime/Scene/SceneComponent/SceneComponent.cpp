#include "SceneComponent.hpp"
#include "../SceneGraph.hpp"

void SceneComponent::set_local_transform(AffineTransform T) {
	localTransform = T;
	parent.update(entity);
}