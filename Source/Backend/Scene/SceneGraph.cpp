#include "../IO/IO.hpp"
#include "../IO/MeshAsset.hpp"
#include "../IO/MeshImporter.hpp"
#include "SceneGraph.hpp"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

using namespace IO;
void SceneGraph::load_from_aiScene(const aiScene* scene) {
	// load meshes onto the asset_manager
	std::unordered_map<uint, entt::entity> mesh_mapping;
	for (UINT i = 0; i < scene->mNumMeshes; i++) {
		mesh_static mesh = IO::load_static_mesh(scene->mMeshes[i]);
		auto asset = assets.load<StaticMeshAsset, mesh_static>(mesh);		
		auto entity = registry.create();
		StaticMeshComponent staticMesh;
		staticMesh.entity = entity;
		staticMesh.mesh_resource = asset;
		staticMesh.name = scene->mMeshes[i]->mName.C_Str();
		registry.emplace<StaticMeshComponent>(entity, staticMesh);
		registry.emplace<SceneComponentTag>(entity, SceneComponentTag::StaticMesh);		
		mesh_mapping[i] = entity;
	}
	// build scene hierarchy
	auto dfs_nodes = [&](auto& func, aiNode* node, entt::entity parent) -> void {
		auto entity = registry.create();
		ComponentCollection component;
		component.entity = entity;
		component.name = node->mName.C_Str();		
		registry.emplace<ComponentCollection>(entity, component);
		registry.emplace<SceneComponentTag>(entity, SceneComponentTag::Collection);		
		add_link(parent, entity);
		for (UINT i = 0; i < node->mNumMeshes; i++) {
			add_link(entity, mesh_mapping[node->mMeshes[i]]);
		}
		for (UINT i = 0; i < node->mNumChildren; i++)
			func(func, node, entity);
	};
	dfs_nodes(dfs_nodes, scene->mRootNode, root);
}

void SceneGraph::log_entites() {
	auto dfs_nodes = [&](auto& func, entt::entity entity) -> void {
		SceneComponentTag tag = registry.get<SceneComponentTag>(entity);
		switch (tag)
		{
		case SceneComponentTag::Root:
			LOG(INFO) << "[ Scene Root ]";
			break;
		case SceneComponentTag::Camera:
			LOG(INFO) << "[ Camera ]";
			LOG(INFO) << " Name: " << registry.get<CameraComponent>(entity).name;
			break;
		case SceneComponentTag::Collection:
			LOG(INFO) << "[ Collection ]";
			LOG(INFO) << " Name: " << registry.get<ComponentCollection>(entity).name;
			break;
		case SceneComponentTag::StaticMesh:
			LOG(INFO) << "[ Static Mesh ]";
			LOG(INFO) << " Name: " << registry.get<StaticMeshComponent>(entity).name;		
			break;
		}
		for (auto child : graph[entity])
			func(func, child);
	};
	dfs_nodes(dfs_nodes, root);
}