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

void SceneImporter::load_aiScene(UploadContext* ctx, SceneImporterAtomicStatus& statusOut, Scene& sceneOut, const aiScene* scene, path_t sceneFolder) {
	RHI::Device* device = ctx->GetParent();
	DefaultTaskThreadPool pool;
	std::mutex import_mutex;
	std::unordered_map<uint, entt::entity> mesh_mapping;	
	statusOut.numToUpload += scene->mNumMeshes;
	for (UINT i = 0; i < scene->mNumMeshes; i++) {
		auto entity = sceneOut.create<AssetMeshComponent>();
		auto handle = sceneOut.create<MeshAsset>();
		AssetMeshComponent& mesh_asset = sceneOut.emplace<AssetMeshComponent>(entity);
		mesh_asset.set_name(scene->mMeshes[i]->mName.C_Str());
		mesh_asset.mesh = handle;
		mesh_mapping[i] = entity;
		pool.push([&](AssetHandle meshHandle, auto meshPtr) {
			LOG(INFO) << "Loading mesh " << meshPtr->mName.C_Str();
			StaticMesh mesh = load_static_mesh(meshPtr);
			std::scoped_lock lock(import_mutex);
			auto& asset = sceneOut.import_asset(meshHandle, device, &mesh);
			asset.Upload(ctx);
			statusOut.numUploaded++;
			}, handle, scene->mMeshes[i]);
	}
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
	// build scene hierarchy
	auto dfs_nodes = [&](auto& func, aiNode* node, entt::entity parent) -> void {
		auto entity = sceneOut.create<SceneCollectionComponent>();
		SceneCollectionComponent& component = sceneOut.emplace<SceneCollectionComponent>(entity);
		component.set_name(node->mName.C_Str());
		// localTransforms are not updated here. This gets invoked in the end of the graph build.
		sceneOut.graph->add_link(parent, entity);
		for (UINT i = 0; i < node->mNumMeshes; i++) {
			auto mesh = scene->mMeshes[node->mMeshes[i]];
			auto meshEntity = sceneOut.create<SceneCollectionComponent>();
			SceneMeshComponent& meshComponent = sceneOut.emplace<SceneMeshComponent>(meshEntity);
			sceneOut.graph->add_link(entity, meshEntity);
			meshComponent.localTransform = SimpleMath::Matrix(XMMATRIX(&node->mTransformation.a1)).Transpose();
			meshComponent.materialAsset = material_mapping[mesh->mMaterialIndex];
			meshComponent.meshAsset = mesh_mapping[node->mMeshes[i]];
		}
		for (UINT i = 0; i < node->mNumChildren; i++)
			func(func, node->mChildren[i], entity);
	};
	dfs_nodes(dfs_nodes, scene->mRootNode, sceneOut.graph->get_root());
	sceneOut.graph->update(sceneOut.graph->get_root(), true); // Compute global transforms once
	pool.wait_and_join(); // ensure pool work is done before finally destructing import_mutex
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
	auto imported = importer.ReadFile(u8path, aiProcess_Triangulate | aiProcess_ConvertToLeftHanded | aiProcess_CalcTangentSpace);
	CHECK(imported && "Failed to import file as a assimp scene!");
	SceneImporter::load_aiScene(ctx, statusOut, sceneOut, imported, sceneFile.parent_path());
}