#include "../../Common/Task.hpp"
#include "../AssetRegistry/IO.hpp"
#include "../AssetRegistry/AssetRegistry.hpp"
#include "../AssetRegistry/MeshAsset.hpp"
#include "../AssetRegistry/MeshImporter.hpp"
#include "../AssetRegistry/ImageImporter.hpp"
#include "../AssetRegistry/ImageAsset.hpp"

#include "SceneGraph.hpp"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

void SceneGraph::load_from_aiScene(const aiScene* scene, path_t sceneFolder) {	
	DefaultTaskThreadPool pool;
	std::mutex import_mutex;
	std::unordered_map<uint, entt::entity> mesh_mapping;
	for (UINT i = 0; i < scene->mNumMeshes; i++) {
		auto entity = registry.create();
		StaticMeshComponent& staticMesh = emplace<StaticMeshComponent>(entity);
		staticMesh.entity = entity;
		staticMesh.name = scene->mMeshes[i]->mName.C_Str();
		mesh_mapping[i] = entity;
		pool.push([&](auto meshPtr) {
			LOG(INFO) << "Loading mesh " << meshPtr->mName.C_Str();
			auto mesh = load_static_mesh(meshPtr);
			std::scoped_lock lock(import_mutex);
			staticMesh.mesh_resource = assets.import(std::move(mesh));
		}, scene->mMeshes[i]);
	}
	auto load_texture = [&](const char* filename) {
		if (scene->GetEmbeddedTexture(filename)) {
			LOG(INFO) << "Loading embeded texture " << filename;			
			auto bmp = load_bitmap_32bpp(reinterpret_cast<uint8_t*>(scene->GetEmbeddedTexture(filename)->pcData), scene->GetEmbeddedTexture(filename)->mWidth);
			std::scoped_lock lock(import_mutex);
			return assets.import(std::move(bmp)); // texture is embeded
		}
		else {
			LOG(INFO) << "Loading filesystem texture " << filename;
			auto realized_path = find_file(filename, sceneFolder);
			CHECK(realized_path.has_value());
			auto bmp = load_bitmap_32bpp(realized_path.value());
			std::scoped_lock lock(import_mutex);
			return assets.import(std::move(bmp)); // texture is a file			
		}
	};
	std::unordered_map<uint, entt::entity> material_mapping;
	for (UINT i = 0; i < scene->mNumMaterials; i++) {
		auto material = scene->mMaterials[i];
		auto entity = registry.create();
		MaterialComponet& materialComponet = emplace<MaterialComponet>(entity);
		material_mapping[i] = entity;
		materialComponet.entity = entity;
		materialComponet.name = material->GetName().C_Str();
		LOG(INFO) << "Loading material " << materialComponet.name;
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_BASE_COLOR) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_BASE_COLOR, j, &path);	
			pool.push([&,path](){ materialComponet.albedoImage = load_texture(path.C_Str()); });
		}
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_NORMALS) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_NORMALS, j, &path);
			pool.push([&, path]() { materialComponet.normalMapImage = load_texture(path.C_Str()); });
		}
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_METALNESS) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_METALNESS, j, &path);
			pool.push([&, path]() { materialComponet.pbrMapImage = load_texture(path.C_Str()); });
			// one map (RGBA) for metal-roughness PBR pipeline (like glTF)
		}
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_EMISSIVE) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_EMISSIVE, j, &path);
			pool.push([&, path]() { materialComponet.emissiveMapImage = load_texture(path.C_Str()); });
		}
	}
	// build scene hierarchy
	auto dfs_nodes = [&](auto& func, aiNode* node, entt::entity parent) -> void {
		auto entity = registry.create();
		CollectionComponent& component = emplace<CollectionComponent>(entity);
		component.entity = entity;
		component.name = node->mName.C_Str();
		add_link(parent, entity);		
		for (UINT i = 0; i < node->mNumMeshes; i++) {
			add_link(entity, mesh_mapping[node->mMeshes[i]]);
			auto mesh = scene->mMeshes[node->mMeshes[i]];
			add_link(mesh_mapping[node->mMeshes[i]], material_mapping[mesh->mMaterialIndex]);
		}
		for (UINT i = 0; i < node->mNumChildren; i++)
			func(func, node->mChildren[i], entity);
	};
	dfs_nodes(dfs_nodes, scene->mRootNode, root);
}

void SceneGraph::log_entites() {	
	auto dfs_nodes = [&](auto& func, entt::entity entity, uint depth) -> void {
		std::string padding;
		for (uint i = 0; i < depth; i++)
			padding += "\t";		
		SceneComponentType tag = registry.get<SceneComponentType>(entity);
		switch (tag)
		{
		case SceneComponentType::Root:
			LOG(INFO) << padding << "[ Scene Root ]";
			break;
		case SceneComponentType::Camera:
			LOG(INFO) << padding << "[ Camera ]";
			LOG(INFO) << padding << " Name: " << registry.get<CameraComponent>(entity).name;
			break;
		case SceneComponentType::Collection:
			LOG(INFO) << padding << "[ Collection ]";
			LOG(INFO) << padding << " Name: " << registry.get<CollectionComponent>(entity).name;
			break;
		case SceneComponentType::StaticMesh:
			LOG(INFO) << padding << "[ Static Mesh ]";
			LOG(INFO) << padding << " Name: " << registry.get<StaticMeshComponent>(entity).name;
			break;
		case SceneComponentType::Material:
			LOG(INFO) << padding << "[ Material ]";
			LOG(INFO) << padding << " Name: " << registry.get<MaterialComponet>(entity).name;
			break;
		}		
		for (auto child : graph[entity])
			func(func, child, depth+1);
	};
	dfs_nodes(dfs_nodes, root, 0);
}