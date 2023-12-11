#pragma once
#include "../../Common/Math.hpp"
class Scene;
class SceneGraph;
class SceneView;
enum class ComponentType {
	Unknown,
	Asset,
	Scene
};
struct Component {
	friend class Scene;
	friend class SceneGraph;
	friend class SceneView;
protected:
	entt::entity entity{ entt::tombstone };
	std::string name{ "<unamed>" };
	const ComponentType type;
public:
	Scene& parent;

	Component(Scene& parent, entt::entity ent, ComponentType type) : parent(parent), entity(ent), type(type) {};

	void set_name(std::string name_) { name = name_; }
	const char* get_name() { return name.c_str(); }
	entt::entity get_entity() { return entity; }
};

template<typename T> concept IsComponet = std::is_base_of<Component, T>::value;