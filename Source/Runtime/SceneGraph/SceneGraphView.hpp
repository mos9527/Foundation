#pragma once
#include "SceneGraph.hpp"
#include "Components/StaticMesh.hpp"
#include "Components/Camera.hpp"

#include "../RHI/RHI.hpp"
#include "../../Runtime/Renderer/Shaders/Shared.h"
class SceneGraphView {
	SceneGraph& scene;
	std::unique_ptr<RHI::Buffer> globalBuffer;
	std::unique_ptr<RHI::Buffer> meshInstancesBuffer;
	std::unique_ptr<RHI::ShaderResourceView> meshInstancesSRV;
	SceneGlobals stats{};
public:
	SceneGraphView(RHI::Device* device, SceneGraph& scene) : scene(scene) {
		globalBuffer = std::make_unique<RHI::Buffer>(device, RHI::Resource::ResourceDesc::GetGenericBufferDesc(sizeof(SceneGlobals), sizeof(SceneGlobals)));
		meshInstancesBuffer = std::make_unique<RHI::Buffer>(device, RHI::Resource::ResourceDesc::GetGenericBufferDesc(MAX_INSTANCE_COUNT * sizeof(SceneMeshInstance), sizeof(SceneMeshInstance)));				
		meshInstancesSRV = std::make_unique<RHI::ShaderResourceView>(
			meshInstancesBuffer.get(), 
			RHI::ShaderResourceViewDesc::GetStructuredBufferDesc(0, MAX_INSTANCE_COUNT, sizeof(SceneMeshInstance))
		);
	};

	auto get_SceneGlobals() { return globalBuffer.get(); }
	auto get_SceneMeshInstances() { return meshInstancesBuffer.get(); }
	auto get_SceneMeshInstancesSRV() { return meshInstancesSRV.get(); }
	auto const& get_SceneStats(){ return stats; }
	void update(uint frameIndex) {
		std::unordered_map<entt::entity, AffineTransform> global_transforms;
		// calculate global transforms
		auto dfs_nodes = [&](auto& func, entt::entity entity, entt::entity parent) -> void {
			auto ptr = scene.try_get_base_ptr(entity);
			if (ptr)
				global_transforms[entity] = global_transforms[parent] * ptr->localTransform;			
			for (auto child : scene.graph[entity])
				func(func, child, entity);
		};
		dfs_nodes(dfs_nodes, scene.root, scene.root);
		// updates for static mesh view
		uint mesh_index = 0;
		std::vector<SceneMeshInstance> meshes;
		// xxx skinned meshes
		for (auto& mesh : scene.registry.storage<StaticMeshComponent>()) {
			auto& asset = scene.assets.get<StaticMeshAsset>(mesh.mesh_resource);
			SceneMeshInstance sceneMesh{};			
			// xxx materials
			sceneMesh.transform = global_transforms[mesh.entity].GetTransforms().Transpose();
			sceneMesh.vertices = D3D12_VERTEX_BUFFER_VIEW{
				.BufferLocation = asset.vertexBuffer->GetGPUAddress(),
				.SizeInBytes = (UINT)asset.vertexBuffer->GetDesc().sizeInBytes(),
				.StrideInBytes = (UINT)asset.vertexBuffer->GetDesc().stride
			};
			sceneMesh.numVertices = asset.numVertices;
			for (int i = 0; i < MAX_LOD_COUNT; i++) {
				sceneMesh.lods[i].indices = D3D12_INDEX_BUFFER_VIEW{
					.BufferLocation = asset.lod_buffers[i].indexBuffer->GetGPUAddress(),
					.SizeInBytes = (UINT)asset.lod_buffers[i].indexBuffer->GetDesc().sizeInBytes(),
					.Format = DXGI_FORMAT_R32_UINT
				};
				sceneMesh.lods[i].meshlets = asset.lod_buffers[i].meshletBuffer->GetGPUAddress();
				sceneMesh.lods[i].meshletVertices = asset.lod_buffers[i].meshletVertexBuffer->GetGPUAddress();
				sceneMesh.lods[i].meshletTriangles = asset.lod_buffers[i].meshletTriangleBuffer->GetGPUAddress();
				sceneMesh.lods[i].numIndices = asset.lod_buffers[i].numIndices;
				sceneMesh.lods[i].numMeshlets = asset.lod_buffers[i].numMeshlets;
			}
			// xxx meshlets
			meshes.push_back(sceneMesh);
			mesh_index++;
		}
		meshInstancesBuffer->Update(meshes.data(), ::size_in_bytes(meshes), 0);
		// update scene globals		
		stats.numMeshInstances = mesh_index;
		// scene camera
		if (scene.registry.any_of<CameraComponent>(scene.active_camera)) {
			auto& camera = scene.get<CameraComponent>(scene.active_camera);
			auto transform = AffineTransform(global_transforms[scene.active_camera]);
			stats.camera = camera.get_struct(transform);
		}
		else {
			LOG(ERROR) << "No active camera set!";
		}
		stats.frameIndex = frameIndex;
		globalBuffer->Update(&stats, sizeof(stats), 0);
	}
};