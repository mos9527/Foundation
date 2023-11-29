#pragma once
#include "SceneComponent.hpp"
#include "../../Shaders/Shared.h"

struct SceneLightComponent : public SceneComponent {
	static const SceneComponentType type = SceneComponentType::Light;
	SceneLightComponent(Scene& scene, entt::entity ent) : SceneComponent(scene, ent, type) {};

	enum LightType {
		Point = SCENE_LIGHT_TYPE_POINT,
		Directional = SCENE_LIGHT_TYPE_DIRECTIONAL,
		AreaQuad = SCENE_LIGHT_TYPE_AREA_QUAD,
		AreaLine = SCENE_LIGHT_TYPE_AREA_LINE
	} lightType;
	float4 color = float4{ 1,1,1,1 };
	float intensity = 1.0f;
	float radius = 1.0f;

	float2 area_Extents = { 1,1 };
	bool area_TwoSided = true;

	float line_Length = 1.0f;
	float line_Radius = 1.0f;
	bool  line_Caps = true;
};
