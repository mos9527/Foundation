#pragma once
#include "../RHI/RHI.hpp"
#include "../../Common/Graph.hpp"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <meshoptimizer.h>
#include <assimp/postprocess.h>
#include <directxtk/SimpleMath.h>
using namespace DirectX::SimpleMath;
enum class NodeTag {
	StaticMesh	
};
struct SceneNode {
	entt::entity parent;
	entt::entity entity;

	std::string name;
	
	Matrix localTransform;
};
/* a rooted graph representing the scene hierarchy */
class SceneGraph : DAG<entt::entity> {
public:
	void remove_node(entt::entity entity) {
		registry.remove<SceneNode>(entity);
		DAG::remove_vertex(entity);
	}
	void remove_link(entt::entity lhs, entt::entity rhs) {
		registry.get<SceneNode>(rhs).parent = entt::tombstone;
		DAG::remove_edge(lhs, rhs);
	}
	void add_link(entt::entity lhs, entt::entity rhs) {		
		auto& rhsNode = registry.get<SceneNode>(rhs);
		CHECK(!registry.valid(rhsNode.parent)); // root graph elements has at most 1 parent
		rhsNode.parent = lhs;
		DAG::add_edge(lhs, rhs);
	}
	void load_from_aiScene(aiScene* scene) {
		auto dfs_nodes = [&](auto& func, aiNode* node, entt::entity parent) -> void {
			auto entity = registry.create();						
			SceneNode sceneNode{
				.parent = entt::tombstone,
				.entity = entity,
				.name = node->mName.C_Str(),
				.localTransform = XMMatrixTranspose(XMMATRIX(&(node->mTransformation.a1)))
			};
			registry.emplace<SceneNode>(entity, sceneNode);
			add_link(parent, entity);

			for (UINT i = 0; i < node->mNumChildren; i++)
				func(func, node, entity);			
		};
		auto root_entity = registry.create();
		dfs_nodes(dfs_nodes, scene->mRootNode, root_entity);		
	}
private:
	using DAG::graph;
	entt::registry registry;
};