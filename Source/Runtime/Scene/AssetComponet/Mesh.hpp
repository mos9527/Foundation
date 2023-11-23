#pragma once
#include "AssetComponent.hpp"

struct MeshAssetComponent : public AssetComponent {
	static const AssetComponentType type = AssetComponentType::Mesh;
	MeshAssetComponent(SceneGraph& parent, entt::entity entity) : AssetComponent(parent, entity, type) {};

	AssetHandle mesh;
#ifdef IMGUI_ENABLED
	virtual void OnImGui();
#endif
};