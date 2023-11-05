#pragma once
#include "../RHI/RHI.hpp"
#include "../../Common/Graph.hpp"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <directxtk/SimpleMath.h>

#include "Common.hpp"
#include "GeometryManager.hpp"
#include "MaterialManager.hpp"
struct SceneNode {
	entt::entity entity;
	std::string name;
	Matrix localTransform;
};
/* a rooted graph representing the scene hierarchy */
class SceneGraph : DAG<entt::entity> {
	using DAG::graph;
	entt::registry registry;
	entt::entity root_entity;

	GeometryManager geometry_manager{ registry };
	MaterialManager material_manager{ registry };
public:
	SceneGraph() {
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
	void load_from_aiScene(RHI::Device* device, RHI::CommandList* cmdList,const aiScene* scene) {
		// loading meshes
		std::unordered_map<uint, entt::entity> mesh_mapping;
		for (UINT i = 0; i < scene->mNumMeshes; i++) {
			IO::mesh_static mesh = IO::load_static_mesh(scene->mMeshes[i]);
			auto entity = geometry_manager.LoadMesh(device, cmdList, mesh);
			registry.get<Geometry>(entity).name = scene->mMeshes[i]->mName.C_Str();
			mesh_mapping[i] = entity;
		}

		auto dfs_nodes = [&](auto& func, aiNode* node, entt::entity parent) -> void {
			auto entity = registry.create();						
			SceneNode sceneNode{
				.entity = entity,
				.name = node->mName.C_Str(),
				.localTransform = XMMatrixTranspose(XMMATRIX(&(node->mTransformation.a1)))
			};
			registry.emplace<SceneNode>(entity, sceneNode);
			registry.emplace<Tag>(entity, Tag::SceneNode);
			add_link(parent, entity);
			for (UINT i = 0; i < node->mNumMeshes; i++) {
				add_link(entity, mesh_mapping[node->mMeshes[i]]);
			}
			for (UINT i = 0; i < node->mNumChildren; i++)
				func(func, node, entity);			
		};
		dfs_nodes(dfs_nodes, scene->mRootNode, root_entity);		
	}
	void log_scene_hierarchy() {
		auto dfs = [&](auto func, entt::entity vertex, uint depth) -> void {			
			switch (registry.get<Tag>(vertex)) {
				case Tag::Root:
					LOG(INFO) << depth << " -- ROOT --";
					break;
				case Tag::SceneNode:
						LOG(INFO) << depth << " - Scene Node " << registry.get<SceneNode>(vertex).name;
						break;
				case Tag::Geometry:
						LOG(INFO) << depth << " - Static Mesh " << registry.get<Geometry>(vertex).name;
						break;
				case Tag::Texture:
						LOG(INFO) << depth << " - Scene Node " << registry.get<Material>(vertex).name;
						break;
			}
			for (auto v : graph[vertex])
				func(func,v,depth + 1);
		};
		dfs(dfs, root_entity, 0);
	}

};