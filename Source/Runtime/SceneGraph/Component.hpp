#pragma once
#include "../RHI/RHI.hpp"
#include "../../Common/Graph.hpp"
#include "../../Common/Math.hpp"

enum class SceneComponentType {
	Unknown,
	Root,
	Collection,
	Camera,
	StaticMesh
};
struct SceneComponent {
	entt::entity entity;
	std::string name;
	AffineTransform localTransform;
};
struct CollectionComponent : public SceneComponent {};

template<typename T> struct SceneComponentTraits {
	static constexpr SceneComponentType type = SceneComponentType::Unknown;
};

template<> struct SceneComponentTraits<CollectionComponent> {
	static constexpr SceneComponentType type = SceneComponentType::Collection;
};

// xxx reflections
