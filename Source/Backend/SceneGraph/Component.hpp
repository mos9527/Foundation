#pragma once
#include "../RHI/RHI.hpp"
#include "../../Common/Graph.hpp"
#include "../../Common/Math.hpp"
#include "SceneGraphViewStructs.h"

struct SceneComponent {
	entt::entity entity;

	std::string name;
	Transform localTransform;
};

typedef SceneComponent ComponentCollection;
template<typename T> struct SceneComponentTraits {};