#pragma once
#include "../Component.hpp"
enum class SceneComponentType {
	Unknown,
	Collection,
	Camera,
	Mesh,
	Light
};
class SceneGraph;
class SceneGraphView;
struct SceneComponent : public Component {	
	friend class SceneGraph;
	friend class SceneGraphView;
private:
	AffineTransform localTransform;
	AffineTransform globalTransform;
	const SceneComponentType type;
public:
	SceneComponent(SceneGraph& parent, entt::entity ent, SceneComponentType type) : Component(parent, ent, ComponentType::Scene), type(type) {};
	
	void set_local_transform(AffineTransform T);
	inline AffineTransform get_global_transform() { return globalTransform; }

	void set_name(std::string name_) {name = name_;}
	const char* get_name() { return name.c_str(); }
	entt::entity get_entity() { return entity; }	

#ifdef IMGUI_ENABLED
	virtual void OnImGui() = 0;
#endif
};

struct SceneCollectionComponent : public SceneComponent {
	static const SceneComponentType type = SceneComponentType::Collection;
	SceneCollectionComponent(SceneGraph& parent, entt::entity ent) : SceneComponent(parent, ent, type) {};
#ifdef IMGUI_ENABLED
	virtual void OnImGui() {
		// Nothing for Collection
	};
#endif
};

template<typename T> concept IsSceneComponent = std::is_base_of<SceneComponent, T>::value;
