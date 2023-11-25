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
#ifdef IMGUI_ENABLED
	virtual void OnImGui() {
		ImGui::ColorEdit4("Color", (float*) & color, ImGuiColorEditFlags_HDR);
		ImGui::SliderFloat("Intensity", &intensity, 0.0f, 100.0f);
		ImGui::SliderFloat("Radius", &radius,0.0f,100.0f);		
		Vector3 direction = Vector3::TransformNormal({ 0,-1,0 }, get_global_transform());
		ImGui::DragFloat3("Direction", (float*)&direction);
	}
#endif
};
