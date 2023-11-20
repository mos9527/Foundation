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
	/* Versioned members begin */
	size_t version;
	AffineTransform globalTransform;
	BoundingBox boundingBox;
	/* Versioned members end */

	entt::entity entity;
	std::string name;
public:
	SceneComponent(entt::entity ent) : entity(ent) {};

	AffineTransform localTransform;
	BoundingBox const& get_bounding_box() { return boundingBox; }
	AffineTransform const& get_global_transform() { return globalTransform; }
	const char* get_name() { return name.c_str(); }
	entt::entity get_entity() { return entity; }
	size_t get_version() { return version; }
	void set_name(std::string name_) {name = name_;}
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
