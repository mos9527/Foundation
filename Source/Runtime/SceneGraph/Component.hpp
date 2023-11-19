#pragma once
#include "../RHI/RHI.hpp"
#include "../../Common/Graph.hpp"
#include "../../Common/Math.hpp"

enum class SceneComponentType {
	Unknown,
	Collection,
	Camera,
	StaticMesh,	
	Material,
	Light
};
class SceneGraph;
class SceneGraphView;
struct SceneComponent {	
	friend class SceneGraph;
	friend class SceneGraphView;
private:
	entt::entity entity;
	std::string name;
public:
	AffineTransform localTransform;
	SceneComponent(entt::entity ent) : entity(ent) {};

	const char* get_name() { return name.c_str(); }
	void set_name(std::string name_) {name = name_;}
	entt::entity get_entity() { return entity; }
#ifdef IMGUI_ENABLED
	virtual void OnImGui() = 0;
#endif
};
struct CollectionComponent : public SceneComponent {
	using SceneComponent::SceneComponent;
#ifdef IMGUI_ENABLED
	virtual void OnImGui() {
		// Nothing for Collection
	};
#endif
};

template<typename T> struct SceneComponentTraits {
	static constexpr SceneComponentType type = SceneComponentType::Unknown;
};

template<> struct SceneComponentTraits<CollectionComponent> {
	static constexpr SceneComponentType type = SceneComponentType::Collection;
};

template<typename T> concept SceneComponentDefined = requires(T t, entt::entity ent) { T(ent); };
// xxx reflections
