#pragma once
#include "SceneComponent.hpp"

struct SceneLightComponent : public SceneComponent {
	static const SceneComponentType type = SceneComponentType::Light;
	SceneLightComponent(Scene& scene, entt::entity ent) : SceneComponent(scene, ent, type) {};

	enum class LightType {
		Point = 0,
		Directional = 1
	} lightType;
	float4 color;
	float intensity;
	float radius;
};
