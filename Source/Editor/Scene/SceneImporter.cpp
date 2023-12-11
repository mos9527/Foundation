#include "SceneImporter.hpp"
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
void aiNode_Reduce(aiNode* node, auto func) {
	auto dfs = [&](auto& dfs_, aiNode* node) {
		if (!func(node)) return;
		for (UINT i = 0; i < node->mNumChildren; i++)
			dfs_(dfs_, node->mChildren[i]);
	};
	dfs(dfs, node);
}
void aiScene_Reduce(const aiScene* scene, auto func) {
	aiNode_Reduce(scene->mRootNode, func);
}

void SceneImporter::load_aiScene(UploadContext* ctx, SceneImporterAtomicStatus& statusOut, Scene& sceneOut, const aiScene* scene, path_t sceneFolder) {
	RHI::Device* device = ctx->GetParent();
	DefaultTaskThreadPool pool;
	std::mutex import_mutex;
	// Parse aiScene hierarchy and build handles	
	std::map<aiNode*, entt::entity> entity_map;
	// Assign root node to graph root
	entity_map[scene->mRootNode] = sceneOut.graph->get_root();
	sceneOut.get<SceneCollectionComponent>(entity_map[scene->mRootNode]).set_name("Asset Importer Scene Root");
	// * 1 * Collect all armatures		
	// Also collects inverse bind transforms of bones.
	std::set<aiNode*> armature_set;
	std::unordered_map<aiNode*, std::unordered_map<std::string, matrix>> boneInvTransforms;
	aiScene_Reduce(scene, [&](aiNode* node) {
		for (UINT i = 0; i < node->mNumMeshes; i++) {
			auto mesh = scene->mMeshes[node->mMeshes[i]];
			if (mesh->HasBones()) {
				aiNode* armature = mesh->mBones[0]->mArmature;
				armature_set.insert(armature);
				for (uint j = 0; j < mesh->mNumBones; j++) {
					aiBone* bone = mesh->mBones[j];
					boneInvTransforms[armature][bone->mName.C_Str()] = SimpleMath::Matrix(XMMATRIX(&bone->mOffsetMatrix.a1)).Transpose();
				}
			}
		}
		return true;
	});
	// * 2 * Create collection per node types
	// * 2.1 * Image Textures & Materials
	auto load_texture = [&](AssetHandle handle, const char* filename, bool linear = false) {
		if (scene->GetEmbeddedTexture(filename)) {
			LOG(INFO) << "Loading embeded texture " << filename;
			Bitmap32bpp bmp = load_bitmap_32bpp(reinterpret_cast<uint8_t*>(scene->GetEmbeddedTexture(filename)->pcData), scene->GetEmbeddedTexture(filename)->mWidth);
			bmp.linear = linear;
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
			bmp.linear = linear;
			std::scoped_lock lock(import_mutex);
			auto& asset = sceneOut.import_asset(handle, device, &bmp); // texture is a file			
			asset.Upload(ctx);
			statusOut.numUploaded++;
			return handle;
		}
	};
	std::unordered_map<uint, entt::entity> material_map;
	for (UINT i = 0; i < scene->mNumMaterials; i++) {
		auto material = scene->mMaterials[i];
		auto entity = sceneOut.create<AssetMaterialComponent>();
		AssetMaterialComponent& materialComponet = sceneOut.emplace<AssetMaterialComponent>(entity);
		material_map[i] = entity;
		materialComponet.set_name(material->GetName().C_Str());
		LOG(INFO) << "Loading Material " << materialComponet.get_name();
		aiColor4D color;
		material->Get(AI_MATKEY_COLOR_DIFFUSE, color);
		materialComponet.albedo = { color.r,color.g,color.b,color.a };
		material->Get(AI_MATKEY_COLOR_EMISSIVE, color);
		materialComponet.emissive = { color.r,color.g,color.b,color.a };
		UINT numAlphaMap = material->GetTextureCount(aiTextureType_OPACITY);
		if (numAlphaMap) materialComponet.alphaMapped = true;
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_BASE_COLOR) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_BASE_COLOR, j, &path);
			pool.push([&, path](AssetHandle handle) { materialComponet.albedoImage = load_texture(handle, path.C_Str(),false); }, sceneOut.create<TextureAsset>());
			statusOut.numToUpload++;
		}
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_NORMALS) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_NORMALS, j, &path);
			pool.push([&, path](AssetHandle handle) { materialComponet.normalMapImage = load_texture(handle, path.C_Str(),true /* normal maps are -ususally- linear! */); }, sceneOut.create<TextureAsset>());
			statusOut.numToUpload++;
		}
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_METALNESS) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_METALNESS, j, &path);
			pool.push([&, path](AssetHandle handle) { materialComponet.pbrMapImage = load_texture(handle, path.C_Str(), true /* https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html */); }, sceneOut.create<TextureAsset>());
			statusOut.numToUpload++;
			// one map (RGBA) for metal-roughness PBR pipeline (like glTF)
		}
		for (UINT j = 0; j < material->GetTextureCount(aiTextureType_EMISSIVE) && j < 1; j++) {
			aiString path; material->GetTexture(aiTextureType_EMISSIVE, j, &path);
			pool.push([&, path](AssetHandle handle) { materialComponet.emissiveMapImage = load_texture(handle, path.C_Str(),false); }, sceneOut.create<TextureAsset>());
			statusOut.numToUpload++;
		}
	}
	// * 2.2 * Static/Keyshape-only meshes
	std::map<uint, AssetHandle> mesh_map;
	/* Static/Skinned(see below) Meshes */
	// Bone-less meshes are also added here
	// Though boned meshes should only appear in Armature nodes (as per filled by aiProcess_PopulateArmatureData)
	// Note that Meshes are always leaf nodes. So they don't go into the entity_map
	auto load_non_skinned_mesh = [&](aiNode* parent, uint meshIndex) {
		aiMesh* mesh = scene->mMeshes[meshIndex];
		if (mesh->HasBones()) return;
		else if (mesh->mNumAnimMeshes) {
			// Keyshape Only mesh
			// Consider this as a skinned mesh too. Though without bone transforms.

			// Use the ordering given by the importer to order keyshapes
			std::unordered_map<std::string, uint> keyshapeNames;
			for (uint j = 0; j < mesh->mNumAnimMeshes; j++)
				keyshapeNames[mesh->mAnimMeshes[j]->mName.C_Str()] = j;

			if (!mesh_map.contains(meshIndex)) {
				auto meshAsset = sceneOut.create<SkinnedMeshAsset>();
				mesh_map[meshIndex] = meshAsset;

				statusOut.numToUpload++;
				pool.push([&](AssetHandle meshHandle, aiMesh* mesh_, std::unordered_map<std::string, uint> keyshapeNames) {
					LOG(INFO) << "Loading Keyshaped Mesh " << mesh_->mName.C_Str();
					SkinnedMesh mesh = load_skinned_mesh(mesh_, {}, keyshapeNames);
					std::scoped_lock lock(import_mutex);
					auto& asset = sceneOut.import_asset(meshHandle, device, &mesh);
					asset.Upload(ctx);
					statusOut.numUploaded++;
					}, meshAsset, mesh, keyshapeNames);
			}
			auto keyshapeWeightEntity = sceneOut.create<AssetKeyshapeTransformComponent>();
			auto& keyshapeWeights = sceneOut.emplace<AssetKeyshapeTransformComponent>(keyshapeWeightEntity);
			keyshapeWeights.setup(device, keyshapeNames);

			auto skinnedMeshEntity = sceneOut.create<SceneSkinnedMeshComponent>();
			auto& skinnedMesh = sceneOut.emplace<SceneSkinnedMeshComponent>(skinnedMeshEntity);
			skinnedMesh.set_name(mesh->mName.C_Str());
			skinnedMesh.meshAsset = mesh_map[meshIndex];
			skinnedMesh.materialComponent = material_map[mesh->mMaterialIndex];
			skinnedMesh.keyshapeTransformComponent = keyshapeWeightEntity;
			sceneOut.graph->add_link(entity_map[parent], skinnedMeshEntity);
		}
		else {
			// Static mesh
			if (!mesh_map.contains(meshIndex)) {
				auto meshAsset = sceneOut.create<StaticMeshAsset>();
				mesh_map[meshIndex] = meshAsset;

				statusOut.numToUpload++;
				pool.push([&](AssetHandle meshHandle, aiMesh* mesh_) {
					LOG(INFO) << "Loading Static Mesh " << mesh_->mName.C_Str();
					StaticMesh mesh = load_static_mesh(mesh_, true);
					std::scoped_lock lock(import_mutex);
					auto& asset = sceneOut.import_asset(meshHandle, device, &mesh);
					asset.Upload(ctx);
					statusOut.numUploaded++;
					}, meshAsset, mesh);
			}

			auto staticMeshEntity = sceneOut.create<SceneStaticMeshComponent>();
			auto& staticMesh = sceneOut.emplace<SceneStaticMeshComponent>(staticMeshEntity);
			staticMesh.set_name(mesh->mName.C_Str());
			staticMesh.meshAsset = mesh_map[meshIndex];
			staticMesh.materialComponent = material_map[mesh->mMaterialIndex];
			sceneOut.graph->add_link(entity_map[parent], staticMeshEntity);
		}
	};
	aiScene_Reduce(scene, [&](aiNode* node) {			
		if (armature_set.contains(node)) return false; // Cut this branch! Dealt with seperately
		if (!entity_map.contains(node)) {
			// Generic collection for non-armature collections
			LOG(INFO) << "New node " << node->mName.C_Str();
			entity_map[node] = sceneOut.create<SceneCollectionComponent>();
			auto& collection = sceneOut.emplace<SceneCollectionComponent>(entity_map[node]);
			collection.set_name(node->mName.C_Str());
		}
		for (uint i = 0; i < node->mNumMeshes; i++) {			
			load_non_skinned_mesh(node, node->mMeshes[i]);
		}		
		if (node != scene->mRootNode)
			sceneOut.graph->add_link(entity_map[node->mParent], entity_map[node]);		
		return true;
	});
	// * 2.3 * Armature
	for (aiNode* armature : armature_set) {
		// [seems like] assimp's imported armature nodes
		// have and only have two children:
		// * one is for the mesh
		// * and the other is for the bone hierarchy
		// So far this has been correct for the glTF models I've tested.
		// For now, this will work.
		entity_map[armature] = sceneOut.create<SceneArmatureComponent>();
		auto& component = sceneOut.emplace<SceneArmatureComponent>(entity_map[armature]);		
		component.set_name(armature->mName.C_Str());		
		std::vector<aiNode*> boneRoots, meshCollections;
		for (uint i = 0; i < armature->mNumChildren; i++) {
			auto child = armature->mChildren[i];
			if (child->mNumMeshes) meshCollections.push_back(child);
			else boneRoots.push_back(child);
		}
		CHECK(boneRoots.size() && meshCollections.size() && "Bad armature hierarchy.");
		aiMesh* mesh0 = scene->mMeshes[meshCollections[0]->mMeshes[0]];
		// Build Keyshape names
		// Armatures binds to one mesh
		// Assimp seperates the mesh with multiple materials
		// But they still share the same keyshape / animMesh namings
		std::unordered_map<std::string, uint> keyshapeNames;
		for (uint j = 0; j < mesh0->mNumAnimMeshes; j++)
			keyshapeNames[mesh0->mAnimMeshes[j]->mName.C_Str()] = j;
		// Build bone names		
		// Also collectes bone local transforms
		std::unordered_map<std::string, uint> boneNames;
		std::unordered_map<std::string, matrix> localMatrices;
		uint boneIndex = 0;
		for (auto bones : boneRoots) {
			aiNode_Reduce(bones, [&](aiNode* node) {			
				boneNames[node->mName.C_Str()] = boneIndex++;
				localMatrices[node->mName.C_Str()] = SimpleMath::Matrix(XMMATRIX(&node->mTransformation.a1)).Transpose();
				return true;
			});
		}
		// Setup the Component
		component.setup(device, boneNames, keyshapeNames);
		// Build bone hierarchy
		for (auto bones : boneRoots) {
			aiNode_Reduce(bones, [&](aiNode* node) {
				if (node == bones) component.add_root_bone(node->mName.C_Str());
				else component.add_bone_hierarchy(node->mParent->mName.C_Str(), node->mName.C_Str());
				return true;
			});
		}
		// Transforms
		auto& invBindMatrices = boneInvTransforms[armature];
		component.setup_inv_bind_matrices(invBindMatrices);
		component.setup_local_matrices(localMatrices);
		// Finishing it up
		component.build();		
		// Adding the meshes
		for (auto& meshes : meshCollections) {
			for (uint j = 0; j < meshes->mNumMeshes; j++) {
				aiMesh* mesh = scene->mMeshes[meshes->mMeshes[j]];
				if (mesh->HasBones() || mesh->mAnimMeshes) {
					if (!mesh_map.contains(meshes->mMeshes[j])) {
						auto meshAsset = sceneOut.create<SkinnedMeshAsset>();
						mesh_map[meshes->mMeshes[j]] = meshAsset;
						statusOut.numToUpload++;
						pool.push([&](AssetHandle meshHandle, aiMesh* srcMesh, std::unordered_map<std::string, uint> boneNames_, std::unordered_map<std::string, uint> keyNames_) {
							LOG(INFO) << "Loading Skinned Mesh " << srcMesh->mName.C_Str() << " For Armature " << armature->mName.C_Str();
							SkinnedMesh mesh = load_skinned_mesh(srcMesh, boneNames_, keyNames_);
							std::scoped_lock lock(import_mutex);
							auto& asset = sceneOut.import_asset(meshHandle, device, &mesh);
							asset.Upload(ctx);
							statusOut.numUploaded++;
						}, meshAsset, mesh, boneNames, keyshapeNames);
					}
					auto skinnedMeshEntity = sceneOut.create<SceneSkinnedMeshComponent>();
					auto& skinnedMesh = sceneOut.emplace<SceneSkinnedMeshComponent>(skinnedMeshEntity);
					skinnedMesh.set_name(mesh->mName.C_Str());
					skinnedMesh.meshAsset = mesh_map[meshes->mMeshes[j]];
					skinnedMesh.materialComponent = material_map[mesh->mMaterialIndex];
					skinnedMesh.keyshapeTransformComponent = component.get_keyshape_transforms();
					skinnedMesh.boneTransformComponent = component.get_bone_transforms();
					sceneOut.graph->add_link(entity_map[armature], skinnedMeshEntity);
				}
				else {
					load_non_skinned_mesh(armature, meshes->mMeshes[j]);
				}
			}
		}
		// Initial update post-build
		component.update();
		sceneOut.graph->add_link(entity_map[armature->mParent], entity_map[armature]);
	}
	// Set local transforms
	for (auto& [node, entity] : entity_map) {
		if (!node || node == scene->mRootNode) continue; // mRootNode somehow has incorrect transforms...
		auto* component = sceneOut.get_base<SceneComponent>(entity);
		component->localTransform = SimpleMath::Matrix(XMMATRIX(&node->mTransformation.a1)).Transpose();	
	}
	sceneOut.graph->update_transform(sceneOut.graph->get_root());
	pool.wait_and_join(); // Ensure the pool is destructed before the mutex. Since there could be unfinished jobs.
}

void SceneImporter::load(UploadContext* ctx, SceneImporterAtomicStatus& statusOut, Scene& sceneOut, path_t sceneFile) {
	AssimpLogger::try_attach();
	Assimp::Importer importer;
	LOG(INFO) << "Reading " << sceneFile;
	CHECK(std::filesystem::exists(sceneFile) && "File does not exisit!");
	std::string u8path = (const char*)sceneFile.u8string().c_str();
	LOG(INFO) << "Importing...";
	auto imported = importer.ReadFile(u8path, aiProcess_Triangulate | aiProcess_ConvertToLeftHanded | aiProcess_CalcTangentSpace | aiProcess_LimitBoneWeights | aiProcess_PopulateArmatureData);	
	CHECK(imported && "Failed to import file as an assimp scene!");
	LOG(INFO) << "Loading...";
	SceneImporter::load_aiScene(ctx, statusOut, sceneOut, imported, sceneFile.parent_path());
	LOG(INFO) << "Scene loaded!";
}