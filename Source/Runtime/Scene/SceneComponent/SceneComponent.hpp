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
	AffineTransform localTransform;
	AffineTransform globalTransform;
	const SceneComponentType type;
	size_t version = 0;
public:
	SceneComponent(Scene& parent, entt::entity ent, SceneComponentType type) : Component(parent, ent, ComponentType::Scene), type(type) {};
	
	void set_local_transform(AffineTransform T);
	inline AffineTransform get_global_transform() { return globalTransform; }

	const size_t get_version() { return version; }
	void set_name(std::string name_) {name = name_;}
	const char* get_name() { return name.c_str(); }
	entt::entity get_entity() { return entity; }	

#ifdef IMGUI_ENABLED
	virtual void OnImGui() = 0;
#endif
};

struct SceneCollectionComponent : public SceneComponent {
	static const SceneComponentType type = SceneComponentType::Collection;
	SceneCollectionComponent(Scene& parent, entt::entity ent) : SceneComponent(parent, ent, type) {};
#ifdef IMGUI_ENABLED
	virtual void OnImGui() {
		// Nothing for Collection
	};
#endif
};

template<typename T> concept IsSceneComponent = std::is_base_of<SceneComponent, T>::value;
