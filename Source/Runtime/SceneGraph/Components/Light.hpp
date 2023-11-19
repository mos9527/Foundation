#pragma once
#include "../Component.hpp"
#include "../../AssetRegistry/IO.hpp"
#include "../../AssetRegistry/AssetRegistry.hpp"
struct LightComponent : public SceneComponent {
	using SceneComponent::SceneComponent;
	enum class LightType {
		Point = 0
	} type;
	float4 color;
	float intensity;
	float radius;

#ifdef IMGUI_ENABLED
	virtual void OnImGui() {
		ImGui::ColorEdit4("Color", (float*) & color, ImGuiColorEditFlags_HDR);
		ImGui::SliderFloat("Intensity", &intensity, 0.0f, 100.0f);
		ImGui::SliderFloat("Radius", &radius,0.0f,100.0f);		
	}
#endif
};

template<> struct SceneComponentTraits<LightComponent> {
	static constexpr SceneComponentType type = SceneComponentType::Light;
};