#pragma once
#include "../Component.hpp"
#include "../../AssetRegistry/IO.hpp"
#include "../../AssetRegistry/AssetRegistry.hpp"

struct StaticMeshComponent : public SceneComponent {
	using SceneComponent::SceneComponent;
	AssetHandle mesh_resource;
	entt::entity material = entt::tombstone;
	int lodOverride = -1;
	bool isOccludee = true; // can be occluded by other geometry
	bool visible = true;
	bool drawBoundingBox = true;
	[[deprecated]] bool isOccluder = true; // occludes other geometry
	// ^ more like not-implemented :(

#ifdef IMGUI_ENABLED
	virtual void OnImGui() {
		ImGui::SliderInt("LOD Override", &lodOverride, -1, MAX_LOD_COUNT);
		ImGui::Checkbox("Occludee", &isOccludee);
		ImGui::Checkbox("Visible", &visible);
		ImGui::Checkbox("Draw Bounding Box", &drawBoundingBox);
	}
#endif
};

template<> struct SceneComponentTraits<StaticMeshComponent> {
	static constexpr SceneComponentType type = SceneComponentType::StaticMesh;
	const char* name = "Static Mesh";
};