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

void SceneGraph::update_global_transform(entt::entity entity, entt::entity parent) {	
	SceneComponent* entity_ptr = try_get_base_ptr(entity);
	SceneComponent* parent_ptr = try_get_base_ptr(parent);	
	CHECK(entity_ptr && parent_ptr);
	entity_ptr->version = version;
	if (entity != parent)
		entity_ptr->globalTransform = parent_ptr->globalTransform * entity_ptr->localTransform;
	else
	{		
		CHECK(entity == root && "Circular dependecy detected");		
		entity_ptr->globalTransform = entity_ptr->localTransform;
	}	
	for (auto child : graph[entity])
		update_global_transform(child, entity);
}
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
		pool.push([&](AssetHandle meshHandle, auto meshPtr) {
			LOG(INFO) << "Loading mesh " << meshPtr->mName.C_Str();
			auto mesh = load_static_mesh(meshPtr);
			std::scoped_lock lock(import_mutex);
			staticMesh.mesh_resource = assets.import(meshHandle, std::move(mesh));
		}, assets.create<StaticMeshAsset>(), scene->mMeshes[i]);
	}
	auto load_texture = [&](AssetHandle handle, const char* filename) {
		if (scene->GetEmbeddedTexture(filename)) {
			LOG(INFO) << "Loading embeded texture " << filename;			
			auto bmp = load_bitmap_32bpp(reinterpret_cast<uint8_t*>(scene->GetEmbeddedTexture(filename)->pcData), scene->GetEmbeddedTexture(filename)->mWidth);
			std::scoped_lock lock(import_mutex);
			return assets.import(handle, std::move(bmp)); // texture is embeded
		}
		else {
			LOG(INFO) << "Loading filesystem texture " << filename;
			auto realized_path = find_file(filename, sceneFolder);
			CHECK(realized_path.has_value());
			auto bmp = load_bitmap_32bpp(realized_path.value());
			std::scoped_lock lock(import_mutex);
			return assets.import(handle, std::move(bmp)); // texture is a file			
		}
	};
	std::unordered_map<uint, entt::entity> material_mapping;
	auto ent = registry.create();
	CollectionComponent & materialCollection = emplace<CollectionComponent>(ent);
	materialCollection.set_name("[Materials]");
	add_link(root, materialCollection.get_entity());
	for (UINT i = 0; i < scene->mNumMaterials; i++) {
		auto material = scene->mMaterials[i];
		auto entity = registry.create();
		MaterialComponet& materialComponet = emplace<MaterialComponet>(entity);		
		add_link(materialCollection.get_entity(), materialComponet.get_entity());
		material_mapping[i] = entity;
		materialComponet.entity = entity;
		materialComponet.name = material->GetName().C_Str();
		LOG(INFO) << "Loading material " << materialComponet.name;
		aiColor4D color;
		material->Get(AI_MATKEY_COLOR_DIFFUSE, color);
		materialComponet.albedo = { color.r,color.g,color.b,color.a };
		material->Get(AI_MATKEY_COLOR_EMISSIVE, color);
		materialComponet.emissive = { color.r,color.g,color.b,color.a };
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_BASE_COLOR) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_BASE_COLOR, j, &path);	
			pool.push([&,path](AssetHandle handle){ materialComponet.albedoImage = load_texture(handle, path.C_Str()); }, assets.create<SDRImageAsset>());
		}
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_NORMALS) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_NORMALS, j, &path);			
			pool.push([&, path](AssetHandle handle) { materialComponet.normalMapImage = load_texture(handle, path.C_Str()); }, assets.create<SDRImageAsset>());
		}
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_METALNESS) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_METALNESS, j, &path);			
			pool.push([&, path](AssetHandle handle) { materialComponet.pbrMapImage = load_texture(handle, path.C_Str()); }, assets.create<SDRImageAsset>());
			// one map (RGBA) for metal-roughness PBR pipeline (like glTF)
		}
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_EMISSIVE) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_EMISSIVE, j, &path);			
			pool.push([&, path](AssetHandle handle) { materialComponet.emissiveMapImage = load_texture(handle, path.C_Str()); }, assets.create<SDRImageAsset>());
		}
	}
	// build scene hierarchy
	auto dfs_nodes = [&](auto& func, aiNode* node, entt::entity parent) -> void {
		auto entity = registry.create();
		CollectionComponent& component = emplace<CollectionComponent>(entity);
		component.entity = entity;
		component.name = node->mName.C_Str();
		component.localTransform = SimpleMath::Matrix(XMMATRIX(&node->mTransformation.a1)).Transpose(); // assimp is again, column major
		add_link(parent, entity);		
		for (UINT i = 0; i < node->mNumMeshes; i++) {
			add_link(entity, mesh_mapping[node->mMeshes[i]]);
			auto mesh = scene->mMeshes[node->mMeshes[i]];
			get<StaticMeshComponent>(mesh_mapping[node->mMeshes[i]]).material = material_mapping[mesh->mMaterialIndex];
		}
		for (UINT i = 0; i < node->mNumChildren; i++)
			func(func, node->mChildren[i], entity);
	};
	dfs_nodes(dfs_nodes, scene->mRootNode, root);	
	pool.wait_and_join(); // ensure pool work is done before finally destructing import_mutex
	LOG(INFO) << "Scene loaded";
	// ~import_mutex(); // implictly called
}

#ifdef IMGUI_ENABLED
void SceneGraph::OnImGui() {	
	auto dfs_nodes = [&](auto& func, entt::entity entity, uint depth) -> void {
		SceneComponentType tag = registry.get<SceneComponentType>(entity);
		SceneComponent* componet = try_get_base_ptr(entity);
		uint stack_count = 0;
		if (ImGui::TreeNode(componet->get_name())) {

			ImGui::SeparatorText("Transforms");
			auto transform_ui = [&](const char** names, AffineTransform transform) {
				Vector3 translation;
				Quaternion rotationQuaternion;
				Vector3 scale;
				Vector3 eulers;
				transform.Decompose(scale, rotationQuaternion, translation);
				eulers = rotationQuaternion.ToEuler();
				ImGui::DragFloat3(names[0], (float*)&translation, 0.001f);
				ImGui::DragFloat3(names[1], (float*)&eulers, XM_PI / 360.0f);
				ImGui::DragFloat3(names[2], (float*)&scale, 0.01f);
				rotationQuaternion = rotationQuaternion.CreateFromYawPitchRoll(eulers);
				return AffineTransform(translation, rotationQuaternion, scale);
			};
			ImGui::SeparatorText("Local");
			const char* localNames[3] = {"Local Translation","Local Euler","Local Scale"};
			const char* globalNames[3] = {"Global Translation","Global Euler", "Global Scale"};
			componet->localTransform = transform_ui(localNames, componet->localTransform);
			ImGui::SeparatorText("Global");
			transform_ui(globalNames,componet->get_global_transform());
			ImGui::SeparatorText("Component");
			componet->OnImGui();

			stack_count++;
			std::vector<entt::entity> entity_ordered(graph[entity].begin(), graph[entity].end()); 
			std::sort(entity_ordered.begin(), entity_ordered.end(), [&](entt::entity lhs, entt::entity rhs) {
				// sort in lexicographicaly descending order
				int lex = strcmp(try_get_base_ptr(lhs)->get_name(), try_get_base_ptr(rhs)->get_name());
				if (lex != 0) return lex < 0;
				// same name. to enforce strict weak ordering, entity id is then used instead
				return lhs < rhs;
			});;
			for (auto child : entity_ordered)
				func(func, child, depth + 1);		
		}		
		while(stack_count--) ImGui::TreePop();
	};
	dfs_nodes(dfs_nodes, root, 0);
};
#endif