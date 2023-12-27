#pragma once
#include "../Component.hpp"
#include "../../../Runtime/Asset/Asset.hpp"

enum class SceneComponentType {
	Unknown,
	Collection,
	Camera,
	StaticMesh,
	SkinnedMesh,
	Light,
	Armature
};
struct SceneImporter;
struct SceneComponent : public Component {	
	friend class Scene;
	friend class SceneGraph;	
	friend struct SceneImporter;
private:
	AffineTransform localTransform = AffineTransform::Identity;
	AffineTransform globalTransform = AffineTransform::Identity;
	BoundingBox boundingBox;

	const SceneComponentType type;

	bool enabled = true;
	bool selected = false;
public:

	SceneComponent(Scene& parent, entt::entity ent, SceneComponentType type) : Component(parent, ent, ComponentType::Scene), type(type) {};
	
	void update();
	void set_local_transform(AffineTransform T);
	void set_enabled(bool enabled);
	void set_selected(bool selected);
	
	inline AffineTransform get_local_transform() const { return localTransform; }
	inline AffineTransform get_global_transform() const { return globalTransform; }
	inline BoundingBox get_bounding_box() const { return boundingBox; }

	inline const SceneComponentType get_type() const { return type; }

	inline bool get_selected() const { return selected; }
	inline bool get_enabled() const { return enabled; }
};

struct SceneCollectionComponent : public SceneComponent {
	static const SceneComponentType type = SceneComponentType::Collection;
	SceneCollectionComponent(Scene& parent, entt::entity ent) : SceneComponent(parent, ent, type) {};
};

template<typename T> concept IsSceneComponent = std::is_base_of<SceneComponent, T>::value;
