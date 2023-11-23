#pragma once
#include "../../pch.hpp"
#include "../../Common/Math.hpp"
class SceneGraph;
enum class ComponentType {
	Unknown,
	Asset,
	Scene
};
struct Component {
	friend class SceneGraph;
	friend class SceneGraphView;
protected:
	entt::entity entity{ entt::tombstone };
	std::string name{ "<unamed>" };
	const ComponentType type;
public:
	SceneGraph& parent;

	Component(SceneGraph& parent, entt::entity ent, ComponentType type) : parent(parent), entity(ent), type(type) {};

	void set_name(std::string name_) { name = name_; }
	const char* get_name() { return name.c_str(); }
	entt::entity get_entity() { return entity; }

#ifdef IMGUI_ENABLED
	virtual void OnImGui() = 0;
#endif
};

template<typename T> concept IsComponet = std::is_base_of<Component, T>::value;