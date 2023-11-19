#pragma once
#include "../Component.hpp"
#include "../../AssetRegistry/IO.hpp"
#include "../../AssetRegistry/AssetRegistry.hpp"
struct LightComponent : public SceneComponent {
	enum class LightType {
		Point = 0
	} type;
	float4 color;
	float intensity;
	float radius;
};

template<> struct SceneComponentTraits<LightComponent> {
	static constexpr SceneComponentType type = SceneComponentType::Light;
};