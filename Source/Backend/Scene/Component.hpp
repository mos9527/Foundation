#pragma once
#include "../RHI/RHI.hpp"
#include "../../Common/Graph.hpp"
#include "../../Common/Math.hpp"
struct SceneComponent {
	entt::entity entity;

	std::string name;
	Transform localTransform;
};

typedef SceneComponent ComponentCollection;