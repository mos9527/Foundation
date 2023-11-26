#pragma once
#include "../Component.hpp"
enum class SceneComponentType {
	Unknown,
	Collection,
	Camera,
	Mesh,
	Light
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
	size_t version = 0;
public:
	SceneComponent(Scene& parent, entt::entity ent, SceneComponentType type) : Component(parent, ent, ComponentType::Scene), type(type) {};
	
	void update(bool associative = false);

	void set_name(std::string name_) {name = name_;}
	void set_local_transform(AffineTransform T);

	inline AffineTransform get_local_transform() { return localTransform; }
	inline AffineTransform get_global_transform() { return globalTransform; }
	inline BoundingBox get_bounding_box() { return boundingBox; }

	const size_t get_version() const { return version; }
	const char* get_name() const { return name.c_str(); }
	entt::entity get_entity() const { return entity; }	
};

struct SceneCollectionComponent : public SceneComponent {
	static const SceneComponentType type = SceneComponentType::Collection;
	SceneCollectionComponent(Scene& parent, entt::entity ent) : SceneComponent(parent, ent, type) {};
};

template<typename T> concept IsSceneComponent = std::is_base_of<SceneComponent, T>::value;
