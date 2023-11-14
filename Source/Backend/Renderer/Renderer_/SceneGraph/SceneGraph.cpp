#include "SceneGraph.hpp"
void SceneGraph::load_from_aiScene(RHI::Device* device, RHI::CommandList* cmdList, RHI::DescriptorHeap* storageHeap, const aiScene* scene) {
	// loading meshes
	std::unordered_map<uint, entt::entity> mesh_mapping;
	for (UINT i = 0; i < scene->mNumMeshes; i++) {
		IO::mesh_static mesh = IO::load_static_mesh(scene->mMeshes[i]);
		auto entity = geometry_manager.LoadMesh(device, cmdList, storageHeap, mesh);
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