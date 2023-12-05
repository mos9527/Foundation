#include "SceneImporter.hpp"

#include <assimp/Importer.hpp>      // C++ importer interface
#include <assimp/scene.h>           // Output data structure
#include <assimp/postprocess.h>
#include "assimp/Logger.hpp"
#include "assimp/DefaultLogger.hpp"
#include "assimp/LogStream.hpp"
class AssimpLogger : public Assimp::LogStream {
public:
	void write(const char* message) {
		LOG(INFO) << message;
	}
	static void try_attach();
	static AssimpLogger* instance;
};
AssimpLogger* AssimpLogger::instance;
void AssimpLogger::try_attach() {
	if (!instance) {
		instance = new AssimpLogger;
		auto serverity = Assimp::Logger::VERBOSE;
		Assimp::DefaultLogger::create("", serverity);
		Assimp::DefaultLogger::get()->attachStream(instance, serverity);
	}
}
// func -> bool. return false to cut the DFS branch
void aiScene_Reduce(const aiScene* scene, auto func) {
	auto dfs = [&](auto& dfs_, aiNode* node) {
		if (!func(node)) return;
		for (UINT i = 0; i < node->mNumChildren; i++)
			dfs_(dfs_, node->mChildren[i]);
	};
	dfs(dfs, scene->mRootNode);
}

void SceneImporter::load_aiScene(UploadContext* ctx, SceneImporterAtomicStatus& statusOut, Scene& sceneOut, const aiScene* scene, path_t sceneFolder) {
	RHI::Device* device = ctx->GetParent();
	DefaultTaskThreadPool pool;
	std::mutex import_mutex;
	/*Queue asset parsing & loading*/
	// Meshes
	// (Static & Skinned)
	statusOut.numToUpload += scene->mNumMeshes;
	auto is_mesh_static = [](aiMesh* mesh) {return mesh->mNumBones == 0 && mesh->mNumAnimMeshes == 0; };
	std::unordered_map<uint, entt::entity> mesh_mapping;	
	for (UINT i = 0; i < scene->mNumMeshes; i++) {
		if (is_mesh_static(scene->mMeshes[i])) {
			// Static
			auto entity = sceneOut.create<AssetStaticMeshComponent>();
			auto handle = sceneOut.create<StaticMeshAsset>();
			AssetStaticMeshComponent& mesh_asset = sceneOut.emplace<AssetStaticMeshComponent>(entity);
			mesh_asset.set_name(scene->mMeshes[i]->mName.C_Str());
			mesh_asset.mesh = handle;
			mesh_mapping[i] = entity;
			pool.push([&](AssetHandle meshHandle, auto meshPtr) {
				LOG(INFO) << "Loading Static Mesh " << meshPtr->mName.C_Str();
				StaticMesh mesh = load_static_mesh(meshPtr, true);
				std::scoped_lock lock(import_mutex);
				auto& asset = sceneOut.import_asset(meshHandle, device, &mesh);
				asset.Upload(ctx);
				statusOut.numUploaded++;
			}, handle, scene->mMeshes[i]);
		}
		else {
			// Skinned and/or has Keyshapes
			auto entity = sceneOut.create<AssetSkinnedMeshComponent>();
			auto handle = sceneOut.create<SkinnedMeshAsset>();
			AssetSkinnedMeshComponent& mesh_asset = sceneOut.emplace<AssetSkinnedMeshComponent>(entity);
			mesh_asset.set_name(scene->mMeshes[i]->mName.C_Str());
			mesh_asset.mesh = handle;
			mesh_mapping[i] = entity;
			pool.push([&](AssetHandle meshHandle, auto meshPtr) {
				LOG(INFO) << "Loading Skinned Mesh " << meshPtr->mName.C_Str();
				SkinnedMesh mesh = load_skinned_mesh(meshPtr);
				std::scoped_lock lock(import_mutex);
				auto& asset = sceneOut.import_asset(meshHandle, device, &mesh);
				asset.Upload(ctx);
				statusOut.numUploaded++;
			}, handle, scene->mMeshes[i]);
		}
	}
	// Image textures
	// And materials
	auto load_texture = [&](AssetHandle handle, const char* filename) {
		if (scene->GetEmbeddedTexture(filename)) {
			LOG(INFO) << "Loading embeded texture " << filename;
			Bitmap32bpp bmp = load_bitmap_32bpp(reinterpret_cast<uint8_t*>(scene->GetEmbeddedTexture(filename)->pcData), scene->GetEmbeddedTexture(filename)->mWidth);
			std::scoped_lock lock(import_mutex);
			auto& asset = sceneOut.import_asset(handle, device, &bmp); // texture is embeded
			asset.Upload(ctx);
			statusOut.numUploaded++;
			return handle;
		}
		else {
			LOG(INFO) << "Loading filesystem texture " << filename;
			auto realized_path = find_file(filename, sceneFolder);
			CHECK(realized_path.has_value());
			auto bmp = load_bitmap_32bpp(realized_path.value());
			std::scoped_lock lock(import_mutex);
			auto& asset = sceneOut.import_asset(handle, device, &bmp); // texture is a file			
			asset.Upload(ctx);
			statusOut.numUploaded++;
			return handle;
		}
	};	
	std::unordered_map<uint, entt::entity> material_mapping;
	for (UINT i = 0; i < scene->mNumMaterials; i++) {
		auto material = scene->mMaterials[i];
		auto entity = sceneOut.create<AssetMaterialComponent>();
		AssetMaterialComponent& materialComponet = sceneOut.emplace<AssetMaterialComponent>(entity);
		material_mapping[i] = entity;
		materialComponet.set_name(material->GetName().C_Str());
		LOG(INFO) << "Loading material " << materialComponet.get_name();
		aiColor4D color;
		material->Get(AI_MATKEY_COLOR_DIFFUSE, color);
		materialComponet.albedo = { color.r,color.g,color.b,color.a };
		material->Get(AI_MATKEY_COLOR_EMISSIVE, color);
		materialComponet.emissive = { color.r,color.g,color.b,color.a };
		UINT numAlphaMap = material->GetTextureCount(aiTextureType_OPACITY);
		if (numAlphaMap) materialComponet.alphaMapped = true;
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_BASE_COLOR) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_BASE_COLOR, j, &path);
			pool.push([&, path](AssetHandle handle) { materialComponet.albedoImage = load_texture(handle, path.C_Str()); }, sceneOut.create<TextureAsset>());
			statusOut.numToUpload++;
		}
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_NORMALS) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_NORMALS, j, &path);
			pool.push([&, path](AssetHandle handle) { materialComponet.normalMapImage = load_texture(handle, path.C_Str()); }, sceneOut.create<TextureAsset>());
			statusOut.numToUpload++;
		}
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_METALNESS) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_METALNESS, j, &path);
			pool.push([&, path](AssetHandle handle) { materialComponet.pbrMapImage = load_texture(handle, path.C_Str()); }, sceneOut.create<TextureAsset>());
			statusOut.numToUpload++;
			// one map (RGBA) for metal-roughness PBR pipeline (like glTF)
		}
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_EMISSIVE) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_EMISSIVE, j, &path);
			pool.push([&, path](AssetHandle handle) { materialComponet.emissiveMapImage = load_texture(handle, path.C_Str()); }, sceneOut.create<TextureAsset>());
			statusOut.numToUpload++;
		}
	}	
	// Scene Hiearchy
	pool.wait_and_join(); // ensure pool work is done & components are constructed
	/*Armatures*/
	// * 1 * Search through all meshes and mark bone names
	std::set<std::string> bone_set;
	aiScene_Reduce(scene, [&](aiNode* node) {
		for (UINT i = 0; i < node->mNumMeshes; i++) {
			auto mesh = scene->mMeshes[node->mMeshes[i]];
			if (mesh->HasBones()) {
				for (UINT j = 0; j < mesh->mNumBones; j++)
					bone_set.insert(mesh->mBones[j]->mName.C_Str());
			}
		}
		return true;
	});
	// * 2 * Find any node that has a depth=1 children that is a bone (matched by name -> bone_set)
	// These would be Armatures		
	std::set<aiNode*> armature_set;
	aiScene_Reduce(scene, [&](aiNode* node) {
		for (UINT i = 0; i < node->mNumChildren; i++) {
			if (bone_set.contains(node->mChildren[i]->mName.C_Str())) {
				armature_set.insert(node);
				return false;
			}
		}
		return true;
	});
	// * 3 * Begin adding onto the scene
	std::map<aiNode*, entt::entity> entity_mapping;	
	entity_mapping[scene->mRootNode] = sceneOut.graph->get_root();
	sceneOut.get<SceneCollectionComponent>(sceneOut.graph->get_root()).set_name("Asset Importer Scene Root");
	auto create_skinned_transforms = [&](AssetSkinnedMeshComponent & meshAssetComponent) {
		auto transformEnitity = sceneOut.create<AssetSkinnedMeshTransformComponent>();
		AssetSkinnedMeshTransformComponent& transformComponent = sceneOut.emplace<AssetSkinnedMeshTransformComponent>(transformEnitity);	
		SkinnedMeshAsset& meshAsset = sceneOut.get<SkinnedMeshAsset>(meshAssetComponent.mesh);
		transformComponent.setup(device, meshAsset.boneNames.size(), meshAsset.keyShapeNames.size());
		return transformEnitity;
	};
	auto add_static_mesh = [&](entt::entity parent, aiNode* node, uint nodeMeshIndex) -> SceneStaticMeshComponent& {
		aiMesh* mesh = scene->mMeshes[node->mMeshes[nodeMeshIndex]];
		CHECK(is_mesh_static(mesh));
		auto meshEntity = sceneOut.create<SceneStaticMeshComponent>();
		SceneStaticMeshComponent& meshComponent = sceneOut.emplace<SceneStaticMeshComponent>(meshEntity);
		meshComponent.materialAsset = material_mapping[mesh->mMaterialIndex];
		meshComponent.meshAsset = mesh_mapping[node->mMeshes[nodeMeshIndex]];
		return meshComponent;
	};
	auto add_skinned_mesh = [&](entt::entity parent, aiNode* node, uint nodeMeshIndex) -> SceneSkinnedMeshComponent& {
		aiMesh* mesh = scene->mMeshes[node->mMeshes[nodeMeshIndex]];
		CHECK(!is_mesh_static(mesh));
		auto meshEntity = sceneOut.create<SceneStaticMeshComponent>();
		SceneSkinnedMeshComponent& meshComponent = sceneOut.emplace<SceneSkinnedMeshComponent>(meshEntity);
		auto transformEnitity = sceneOut.create<AssetSkinnedMeshTransformComponent>();
		meshComponent.materialAsset = material_mapping[mesh->mMaterialIndex];
		meshComponent.meshAsset = mesh_mapping[node->mMeshes[nodeMeshIndex]];
		return meshComponent;
	};
	aiScene_Reduce(scene, [&](aiNode* node) {
		// Visit Root's children
		if (node == scene->mRootNode) {
			return true;
		}
		// Armature -> Armature Component for the collection	
		if (armature_set.contains(node)) {
			auto entity = sceneOut.create<SceneArmatureComponent>();
			SceneArmatureComponent& armature = sceneOut.emplace<SceneArmatureComponent>(entity);
			armature.localTransform = SimpleMath::Matrix(XMMATRIX(&node->mTransformation.a1)).Transpose();
			for (UINT i = 0; i < node->mNumChildren; i++) {
				// Both Bones are Meshes are at depth=1
				// Add the meshes
				if (node->mChildren[i]->mNumMeshes) {
					aiNode* armMeshes = node->mChildren[i];					
					for (UINT j = 0; j < armMeshes->mNumMeshes; j++) {
						auto& meshComponent = add_skinned_mesh(entity, armMeshes, j);
						if (j == 0) {
							// It's guaranteed all the child meshes of the armature
							// share the same set & count of bones / keyshapes						
							AssetSkinnedMeshComponent& meshAssetComponent = sceneOut.get<AssetSkinnedMeshComponent>(meshComponent.meshAsset);		
							SkinnedMeshAsset& meshAsset = sceneOut.get<SkinnedMeshAsset>(meshAssetComponent.mesh);
							armature.setup(device, meshAsset);
						}						
						// Let all the meshes share armature's transform set
						meshComponent.transformAsset = armature.get_transform_asset();
						sceneOut.graph->add_link(armature.get_entity(), meshComponent.get_entity());
					}
					break;
				}
			}
			for (UINT i = 0; i < node->mNumChildren; i++) {
				// Add the bones
				// We don't use the scene graph for the bone hierarchy
				// Those goes to the armature
				if (bone_set.contains(node->mChildren[i]->mName.C_Str())) {
					// Must be the root bone
					aiNode* rootBone = node->mChildren[i];
					armature.set_root_bone(rootBone->mName.C_Str());
					// Build the hierarchy
					auto dfs_bones = [&](auto& dfs_bones_, aiNode* bone) -> void {		
						armature.set_bone_local_transform(bone->mName.C_Str(), SimpleMath::Matrix(XMMATRIX(&bone->mTransformation.a1)).Transpose());
						for (uint j = 0; j < bone->mNumChildren; j++) {
							armature.add_bone_hierarchy(bone->mName.C_Str(), bone->mChildren[j]->mName.C_Str());
							armature.set_bone_local_transform(bone->mChildren[j]->mName.C_Str(), SimpleMath::Matrix(XMMATRIX(&bone->mChildren[j]->mTransformation.a1)).Transpose());
							dfs_bones_(dfs_bones_, bone->mChildren[j]);
						}
					};
					dfs_bones(dfs_bones, rootBone);
				}
			}
			entity_mapping[node] = entity;
			sceneOut.graph->add_link(entity_mapping[node->mParent], entity);
			armature.update();
			return false;
		}
		// Other grouping -> SceneCollectionComponent to handle collections
		else {
			auto entity = sceneOut.create<SceneCollectionComponent>();
			SceneCollectionComponent& component = sceneOut.emplace<SceneCollectionComponent>(entity);
			component.localTransform = SimpleMath::Matrix(XMMATRIX(&node->mTransformation.a1)).Transpose();
			// Non-armature component meshes
			for (UINT i = 0; i < node->mNumMeshes; i++) {
				if (is_mesh_static(scene->mMeshes[node->mMeshes[i]])) {
					auto& component = add_static_mesh(entity, node, i);
					sceneOut.graph->add_link(entity, component.get_entity());
				}
				else {
					auto& component = add_skinned_mesh(entity, node, i);
					AssetSkinnedMeshComponent& meshAssetComponent = sceneOut.get<AssetSkinnedMeshComponent>(component.meshAsset);
					component.transformAsset = create_skinned_transforms(meshAssetComponent);
					sceneOut.graph->add_link(entity, component.get_entity());
				}
			}
			entity_mapping[node] = entity;
			sceneOut.graph->add_link(entity_mapping[node->mParent], entity);
			return true;
		}
	});
	auto dfs_nodes = [&](auto& func, aiNode* node, entt::entity parent) -> void {
		auto entity = sceneOut.create<SceneCollectionComponent>();
		SceneCollectionComponent& component = sceneOut.emplace<SceneCollectionComponent>(entity);
		component.localTransform = SimpleMath::Matrix(XMMATRIX(&node->mTransformation.a1)).Transpose();
		component.set_name(node->mName.C_Str());		
		LOG(INFO) << node->mName.C_Str();
		sceneOut.graph->add_link(parent, entity);		
		for (UINT i = 0; i < node->mNumChildren; i++)
			func(func, node->mChildren[i], entity);
	};
	dfs_nodes(dfs_nodes, scene->mRootNode, sceneOut.graph->get_root());
	sceneOut.graph->update_transform(sceneOut.graph->get_root()); // Calculate global transforms after everything's in place
	LOG(INFO) << "Scene loaded";
	statusOut.numToUpload = true;
	// ~import_mutex(); // implictly called
}

void SceneImporter::load(UploadContext* ctx, SceneImporterAtomicStatus& statusOut, Scene& sceneOut, path_t sceneFile) {
	AssimpLogger::try_attach();
	Assimp::Importer importer;
	LOG(INFO) << "Loading " << sceneFile;
	CHECK(std::filesystem::exists(sceneFile) && "File does not exisit!");
	std::string u8path = (const char*)sceneFile.u8string().c_str();
	auto imported = importer.ReadFile(u8path, aiProcess_Triangulate | aiProcess_ConvertToLeftHanded | aiProcess_CalcTangentSpace | aiProcess_LimitBoneWeights | aiProcess_PopulateArmatureData);
	CHECK(imported && "Failed to import file as an assimp scene!");
	SceneImporter::load_aiScene(ctx, statusOut, sceneOut, imported, sceneFile.parent_path());
}