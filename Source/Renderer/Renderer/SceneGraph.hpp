#pragma once
#include "../RHI/RHI.hpp"
#include "../../Common/Graph.hpp"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <directxtk/SimpleMath.h>

#include "../Common.hpp"
#include "GeometryManager.hpp"
#include "MaterialManager.hpp"
struct SceneNode {
	entt::entity entity;
	std::string name;
	DirectX::SimpleMath::Matrix localTransform;
};
/* a rooted graph representing the scene hierarchy */
class SceneGraph : DAG<entt::entity> {
	using DAG::graph;
	entt::registry& registry;
	GeometryManager& geometry_manager;
	MaterialManager& material_manager;

	entt::entity root_entity;
public:
	SceneGraph(entt::registry& _registry, GeometryManager& _geometry_manager, MaterialManager& _material_manager) : registry(_registry), geometry_manager(_geometry_manager), material_manager(_material_manager) {
		root_entity = registry.create();
		registry.emplace<Tag>(root_entity, Tag::Root);
	};
	void remove_node(entt::entity entity) {
		registry.remove<SceneNode>(entity);
		DAG::remove_vertex(entity);
	}
	void remove_link(entt::entity lhs, entt::entity rhs) {
		DAG::remove_edge(lhs, rhs);
	}
	void add_link(entt::entity lhs, entt::entity rhs) {		
		DAG::add_edge(lhs, rhs);
	}
	void load_from_aiScene(RHI::Device* device, RHI::CommandList* cmdList, const aiScene* scene);	
};