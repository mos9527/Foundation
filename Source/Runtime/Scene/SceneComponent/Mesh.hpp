#pragma once
#include "SceneComponent.hpp"
#include "../../Asset/AssetRegistry.hpp"

struct SceneMeshComponent : public SceneComponent {
	static const SceneComponentType type = SceneComponentType::Mesh;
	SceneMeshComponent(Scene& scene, entt::entity ent) : SceneComponent(scene, ent, type) {};

	entt::entity meshAsset = entt::tombstone;
	entt::entity materialAsset = entt::tombstone;

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
