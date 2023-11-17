#include "../AssetRegistry/IO.hpp"
#include "../AssetRegistry/AssetRegistry.hpp"
#include "../AssetRegistry/MeshAsset.hpp"
#include "../AssetRegistry/MeshImporter.hpp"
#include "SceneGraph.hpp"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

void SceneGraph::load_from_aiScene(const aiScene* scene) {
	// load meshes onto the asset_manager
	std::unordered_map<uint, entt::entity> mesh_mapping;
	for (UINT i = 0; i < scene->mNumMeshes; i++) {
		mesh_static mesh = load_static_mesh(scene->mMeshes[i]);
		auto asset = assets.import(mesh);
		auto entity = registry.create();
		StaticMeshComponent staticMesh;
		staticMesh.entity = entity;
		staticMesh.mesh_resource = asset;
		staticMesh.name = scene->mMeshes[i]->mName.C_Str();
		emplace<StaticMeshComponent>(entity, staticMesh);		
		mesh_mapping[i] = entity;
	}
	// build scene hierarchy
	auto dfs_nodes = [&](auto& func, aiNode* node, entt::entity parent) -> void {
		auto entity = registry.create();
		CollectionComponent component;
		component.entity = entity;
		component.name = node->mName.C_Str();		
		emplace<CollectionComponent>(entity, component);

		add_link(parent, entity);
		for (UINT i = 0; i < node->mNumMeshes; i++) {
			add_link(entity, mesh_mapping[node->mMeshes[i]]);
		}
		for (UINT i = 0; i < node->mNumChildren; i++)
			func(func, node->mChildren[i], entity);
	};
	dfs_nodes(dfs_nodes, scene->mRootNode, root);
}

void SceneGraph::log_entites() {
	auto dfs_nodes = [&](auto& func, entt::entity entity) -> void {
		SceneComponentType tag = registry.get<SceneComponentType>(entity);
		switch (tag)
		{
		case SceneComponentType::Root:
			LOG(INFO) << "[ Scene Root ]";
			break;
		case SceneComponentType::Camera:
			LOG(INFO) << "[ Camera ]";
			LOG(INFO) << " Name: " << registry.get<CameraComponent>(entity).name;
			break;
		case SceneComponentType::Collection:
			LOG(INFO) << "[ Collection ]";
			LOG(INFO) << " Name: " << registry.get<CollectionComponent>(entity).name;
			break;
		case SceneComponentType::StaticMesh:
			LOG(INFO) << "[ Static Mesh ]";
			LOG(INFO) << " Name: " << registry.get<StaticMeshComponent>(entity).name;		
			break;
		}
		for (auto child : graph[entity])
			func(func, child);
	};
	dfs_nodes(dfs_nodes, root);
}