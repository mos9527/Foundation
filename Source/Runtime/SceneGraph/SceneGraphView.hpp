#pragma once
#include "SceneGraph.hpp"
#include "Components/StaticMesh.hpp"
#include "Components/Camera.hpp"

#include "../RHI/RHI.hpp"
#include "../../Runtime/Renderer/Shaders/Shared.h"
#include "../AssetRegistry/MeshAsset.hpp"

class SceneGraphView {
	SceneGraph& scene;
	std::unique_ptr<RHI::Buffer> globalBuffer;
	std::unique_ptr<RHI::Buffer> meshInstancesBuffer;
	std::unique_ptr<RHI::Buffer> meshMaterialsBuffer;

	SceneGlobals stats{};
	std::vector<SceneMeshInstance> meshes;
	std::vector<SceneMaterial> materials;
public:
	SceneGraphView(RHI::Device* device, SceneGraph& scene) : scene(scene) {
		globalBuffer = std::make_unique<RHI::Buffer>(device, RHI::Resource::ResourceDesc::GetGenericBufferDesc(sizeof(SceneGlobals), sizeof(SceneGlobals)));
		meshInstancesBuffer = std::make_unique<RHI::Buffer>(device, RHI::Resource::ResourceDesc::GetGenericBufferDesc(MAX_INSTANCE_COUNT * sizeof(SceneMeshInstance), sizeof(SceneMeshInstance)));				
		meshMaterialsBuffer = std::make_unique<RHI::Buffer>(device, RHI::Resource::ResourceDesc::GetGenericBufferDesc(MAX_MATERIAL_COUNT * sizeof(SceneMaterial), sizeof(SceneMaterial)));
	};

	auto get_SceneGlobalsBuffer() { return globalBuffer.get(); }
	auto get_SceneMeshInstancesBuffer() { return meshInstancesBuffer.get(); }
	auto get_SceneMeshMaterialsBuffer() { return meshMaterialsBuffer.get(); }
	auto const& get_SceneGlobals() { return stats; }
	auto const& get_SceneInstances() { return meshes; }
	auto const& get_SceneMaterials() { return materials; }
	void update(uint viewportWidth, uint viewportHeight, uint frameIndex) {
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
		// xxx skinned meshes 
		meshes.clear(); materials.clear();
		for (auto& mesh : scene.registry.storage<StaticMeshComponent>()) {
			auto& asset = scene.assets.get<StaticMeshAsset>(mesh.mesh_resource);
			SceneMeshInstance sceneMesh{};					
			auto& transform = global_transforms[mesh.entity];
			sceneMesh.transform = transform.GetTransforms().Transpose();
			sceneMesh.transformInvTranspose = transform.GetTransforms().Invert()/*.Transpose().Transpose()*/;
			sceneMesh.vertices = D3D12_VERTEX_BUFFER_VIEW{
				.BufferLocation = asset.vertexBuffer->GetGPUAddress(),
				.SizeInBytes = (UINT)asset.vertexBuffer->GetDesc().sizeInBytes(),
				.StrideInBytes = (UINT)asset.vertexBuffer->GetDesc().stride
			};
			sceneMesh.numVertices = asset.numVertices;
			for (int i = 0; i < MAX_LOD_COUNT; i++) {
				sceneMesh.lods[i].indices = D3D12_INDEX_BUFFER_VIEW{
					.BufferLocation = asset.lods[i].indexBuffer->GetGPUAddress(),
					.SizeInBytes = (UINT)asset.lods[i].indexBuffer->GetDesc().sizeInBytes(),
					.Format = DXGI_FORMAT_R32_UINT
				};
				sceneMesh.lods[i].meshlets = asset.lods[i].meshletBuffer->GetGPUAddress();
				sceneMesh.lods[i].meshletVertices = asset.lods[i].meshletVertexBuffer->GetGPUAddress();
				sceneMesh.lods[i].meshletTriangles = asset.lods[i].meshletTriangleBuffer->GetGPUAddress();
				sceneMesh.lods[i].numIndices = asset.lods[i].numIndices;
				sceneMesh.lods[i].numMeshlets = asset.lods[i].numMeshlets;
			}			
			// materials
			auto pMaterial = scene.try_get_first_child_of<MaterialComponet>(mesh.entity);
			if (pMaterial) {
				SceneMaterial material;		
				auto try_set_heap_handle = [&](auto handle, auto& dest) {
					auto ptr = scene.assets.try_get_base_image_asset(handle);
					if (ptr && ptr->textureSrv)
						dest = ptr->textureSrv->descriptor.get_heap_handle();
					else
						dest = INVALID_HEAP_HANDLE;
				};
				try_set_heap_handle(pMaterial->albedoImage, material.albedoMap);
				try_set_heap_handle(pMaterial->normalMapImage, material.normalMap);
				try_set_heap_handle(pMaterial->pbrMapImage, material.pbrMap);
				try_set_heap_handle(pMaterial->emissiveMapImage, material.emissiveMap);
				material.albedo = pMaterial->albedo;
				material.pbr = pMaterial->pbr;
				material.emissive = pMaterial->emissive;
				sceneMesh.materialIndex = materials.size();
				materials.push_back(material);
			}
			meshes.push_back(sceneMesh);
		}
		meshInstancesBuffer->Update(meshes.data(), ::size_in_bytes(meshes), 0);
		meshMaterialsBuffer->Update(materials.data(), ::size_in_bytes(materials), 0);
		// update scene globals		
		stats.numMeshInstances = meshes.size();
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
		stats.frameDimension.x = viewportWidth;
		stats.frameDimension.y = viewportHeight;
		globalBuffer->Update(&stats, sizeof(stats), 0);
	}
};