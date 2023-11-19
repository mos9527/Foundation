#pragma once
#include "../Component.hpp"
#include "../../AssetRegistry/IO.hpp"
#include "../../AssetRegistry/AssetRegistry.hpp"

struct StaticMeshComponent : public SceneComponent {
	using SceneComponent::SceneComponent;

	AssetHandle mesh_resource;
	entt::entity material = entt::tombstone;
	int lodOverride = -1;
#ifdef IMGUI_ENABLED
	virtual void OnImGui() {
		ImGui::SliderInt("LOD Override", &lodOverride, -1, MAX_LOD_COUNT);
	}
#endif
};

template<> struct SceneComponentTraits<StaticMeshComponent> {
	static constexpr SceneComponentType type = SceneComponentType::StaticMesh;
	const char* name = "Static Mesh";
};