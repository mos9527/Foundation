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

void SceneGraph::load_from_aiScene(const aiScene* scene) {	
	std::unordered_map<uint, entt::entity> mesh_mapping;
	for (UINT i = 0; i < scene->mNumMeshes; i++) {
		auto asset = assets.import(load_static_mesh(scene->mMeshes[i]));
		auto entity = registry.create();
		StaticMeshComponent staticMesh;
		staticMesh.entity = entity;
		staticMesh.mesh_resource = asset;
		staticMesh.name = scene->mMeshes[i]->mName.C_Str();
		emplace<StaticMeshComponent>(entity, staticMesh);		
		mesh_mapping[i] = entity;
	}
	std::unordered_map<std::string, AssetHandle> texture_mapping;
	for (UINT i = 0; i < scene->mNumTextures; i++) {
		auto texture = scene->mTextures[i];
		AssetHandle asset;
		if (scene->GetEmbeddedTexture(texture->mFilename.C_Str()))			
			asset = assets.import(load_bitmap_8bpp(reinterpret_cast<uint8_t*>(texture->pcData), texture->mWidth));
		else
			asset = assets.import(load_bitmap_8bpp(path_t(texture->mFilename.C_Str()))); // texture is a file		
		texture_mapping[texture->mFilename.C_Str()] = asset;
	}
	std::unordered_map<uint, entt::entity> material_mapping;
	for (UINT i = 0; i < scene->mNumMaterials; i++) {
		auto material = scene->mMaterials[i];
		auto entity = registry.create();
		MaterialComponet materialComponet;
		materialComponet.entity = entity;
		materialComponet.name = material->GetName().C_Str();
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_BASE_COLOR) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_BASE_COLOR, j, &path);
			materialComponet.albedoImage = texture_mapping[path.C_Str()];
		}
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_NORMALS) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_NORMALS, j, &path);
			materialComponet.normalMapImage = texture_mapping[path.C_Str()];
		}
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_METALNESS) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_METALNESS, j, &path);
			materialComponet.pbrMapImage = texture_mapping[path.C_Str()]; // one map (RGBA) for metal-roughness PBR pipeline (like glTF)
		}
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_EMISSIVE) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_EMISSIVE, j, &path);
			materialComponet.emissiveMapImage = texture_mapping[path.C_Str()];
		}
		material_mapping[i] = entity;
		emplace<MaterialComponet>(entity, materialComponet);
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
			auto mesh = scene->mMeshes[node->mMeshes[i]];
			add_link(mesh_mapping[node->mMeshes[i]], material_mapping[mesh->mMaterialIndex]);
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
		case SceneComponentType::Material:
			LOG(INFO) << "[ Material ]";
			LOG(INFO) << " Name: " << registry.get<MaterialComponet>(entity).name;
			break;
		}
		for (auto child : graph[entity])
			func(func, child);
	};
	dfs_nodes(dfs_nodes, root);
}