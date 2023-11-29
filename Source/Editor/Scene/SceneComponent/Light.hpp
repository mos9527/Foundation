#pragma once
#include "SceneComponent.hpp"
#include "../../Shaders/Shared.h"

struct SceneLightComponent : public SceneComponent {
	static const SceneComponentType type = SceneComponentType::Light;
	SceneLightComponent(Scene& scene, entt::entity ent) : SceneComponent(scene, ent, type) {};

	enum LightType {
		Point = SCENE_LIGHT_TYPE_POINT,
		Spot = SCENE_LIGHT_TYPE_DIRECTIONAL,
		Directional = SCENE_LIGHT_TYPE_DIRECTIONAL,
		AreaQuad = SCENE_LIGHT_TYPE_AREA_QUAD,
		AreaLine = SCENE_LIGHT_TYPE_AREA_LINE,
		AreaDisk = SCENE_LIGHT_TYPE_AREA_DISK
	} lightType;

	float intensity = 1.0f;
	float4 color = { 1,1,1,1 };
	/* Spot / Point Light */
	float spot_point_radius = 1.0f;
	/* Spot Light */
	float spot_size_rad = XM_PIDIV4;   // outer
	float spot_size_blend = 0.0f; // inner = size * (1 - blend)
	/* Quad/Disk Area Light */
	float2 area_quad_disk_extents = { 1, 1 };
	bool area_quad_disk_twoSided = true;
	/* Line Area Light */
	float area_line_length = 1.0f;
	float area_line_radius = 1.0f;
	bool  area_line_caps = false;
};
