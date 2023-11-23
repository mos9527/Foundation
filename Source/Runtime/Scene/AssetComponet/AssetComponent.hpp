#pragma once
#include "../Component.hpp"
#include "../../AssetRegistry/Asset.hpp"

enum class AssetComponentType {	
	Unknown,
	Mesh,
	Material
};
class SceneGraph;
class SceneGraphView;
struct AssetComponent : public Component {
	friend class SceneGraph;
	friend class SceneGraphView;	
	const AssetComponentType type;
public:	
	AssetComponent(SceneGraph& parent, entt::entity entity, AssetComponentType type) : Component(parent, entity, ComponentType::Asset), type(type) {};

#ifdef IMGUI_ENABLED
	virtual void OnImGui() = 0;
#endif
};

template<typename T> concept IsAssetComponent = std::is_base_of<AssetComponent, T>::value;