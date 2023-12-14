#pragma once
#include "../Component.hpp"
#include "../../../Runtime/Asset/Asset.hpp"

enum class AssetComponentType {	
	Unknown,
	Material,
	HDRIProbe,
	Data
};
struct AssetComponent : public Component {
	friend class Scene;
	friend class SceneGraph;
	friend class SceneView;	
	const AssetComponentType type;
public:	
	AssetComponent(Scene& parent, entt::entity entity, AssetComponentType type) : Component(parent, entity, ComponentType::Asset), type(type) {};
	inline void update() { version++;  }
};

template<typename T> concept IsAssetComponent = std::is_base_of<AssetComponent, T>::value;