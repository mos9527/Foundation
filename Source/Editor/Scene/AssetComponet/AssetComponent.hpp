#pragma once
#include "../Component.hpp"
#include "../../../Runtime/Asset/Asset.hpp"

enum class AssetComponentType {	
	Unknown,
	StaticMesh,
	SkinnedMesh,
	Material,
	Data
};
struct AssetComponent : public Component {
	friend class Scene;
	friend class SceneGraph;
	friend class SceneView;	
	const AssetComponentType type;
public:	
	AssetComponent(Scene& parent, entt::entity entity, AssetComponentType type) : Component(parent, entity, ComponentType::Asset), type(type) {};
};

template<typename T> concept IsAssetComponent = std::is_base_of<AssetComponent, T>::value;