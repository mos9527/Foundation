#pragma once
#include "SceneGraph.hpp"
#include "SceneGraphViewStructs.h"

#include "Components/StaticMesh.hpp"
#include "Components/Camera.hpp"
#include "../IO/MeshAsset.hpp"

class SceneGraphView {
	SceneGraph& graph;
	std::unique_ptr<RHI::Buffer> statsBuffer;
	std::unique_ptr<RHI::Buffer> instancesBuffer;	
public:
	SceneGraphView(RHI::Device* device, SceneGraph& graph) : graph(graph) {
		statsBuffer = std::make_unique<RHI::Buffer>(device, RHI::Resource::ResourceDesc::GetGenericBufferDesc(sizeof(SceneStats), sizeof(SceneStats)));
		instancesBuffer = std::make_unique<RHI::Buffer>(device, RHI::Resource::ResourceDesc::GetGenericBufferDesc(MAX_INSTANCE_COUNT * sizeof(SceneMesh), sizeof(SceneMesh)));		
	};
	void update() {
		std::unordered_map<entt::entity, Matrix> global_transforms;
		// calculate global transforms
		auto dfs_nodes = [&](auto& func, entt::entity entity, entt::entity parent) -> void {
			auto ptr = graph.try_get_base_ptr(entity);
			if (ptr)
				global_transforms[entity] = global_transforms[parent] * ptr->localTransform;			
			for (auto child : graph.graph[entity])
				func(func, child, entity);
		};
		dfs_nodes(dfs_nodes, graph.root, graph.root);
		// updates for static mesh view
		uint mesh_index = 0;
		std::vector<SceneMesh> meshes;
		// xxx skinned meshes
		for (auto& mesh : graph.registry.storage<StaticMeshComponent>()) {
			auto& asset = graph.assets.get<IO::StaticMeshAsset>(mesh.mesh_resource);
			SceneMesh sceneMesh;
			// xxx materials
			sceneMesh.type = MESH_STATIC;
			sceneMesh.transform = global_transforms[mesh.entity].Transpose(); // HLSL uses col major while directx math is row major
			sceneMesh.vertices = D3D12_VERTEX_BUFFER_VIEW{
				.BufferLocation = asset.vertexBuffer->GetGPUAddress(),
				.SizeInBytes = (UINT)asset.vertexBuffer->GetDesc().sizeInBytes(),
				.StrideInBytes = (UINT)asset.vertexBuffer->GetDesc().stride
			};
			for (int i = 0; i < MAX_LOD_COUNT; i++) {
				sceneMesh.indicesLod[i] = D3D12_INDEX_BUFFER_VIEW{
					.BufferLocation = asset.lod_buffers[i].indexBuffer->GetGPUAddress(),
					.SizeInBytes = (UINT)asset.lod_buffers[i].indexBuffer->GetDesc().sizeInBytes(),
					.Format = DXGI_FORMAT_R32_FLOAT
				};
			}
			// xxx meshlets
			meshes.push_back(sceneMesh);
			mesh_index++;
		}
		instancesBuffer->Update(meshes.data(), ::size_in_bytes(meshes), 0);
		SceneStats stats{};
		stats.numMeshes = mesh_index;
		statsBuffer->Update(&stats, sizeof(stats), 0);
	}
};