#pragma once
#include "SceneGraph.hpp"
#include "Components/StaticMesh.hpp"
#include "Components/Camera.hpp"

#include "../RHI/RHI.hpp"
#include "../../Runtime/Renderer/Shaders/Shared.h"
#include "../AssetRegistry/MeshAsset.hpp"

class SceneGraphView {
	std::unique_ptr<RHI::Buffer> globalBuffer;
	std::unique_ptr<RHI::Buffer> meshInstancesBuffer;
	std::unique_ptr<RHI::Buffer> meshMaterialsBuffer;
	std::unique_ptr<RHI::Buffer> lightBuffer;

	SceneGlobals stats{};
	std::vector<SceneMeshInstance> meshes;
	std::vector<SceneMaterial> materials;
	std::vector<SceneLight> lights;
public:
	struct FrameData {
		SceneGraph* scene;
		uint viewportWidth;
		uint viewportHeight;
		uint frameIndex;
		uint frameFlags;
		uint backBufferIndex;
	};
	SceneGraphView(RHI::Device* device) {
		globalBuffer = std::make_unique<RHI::Buffer>(device, RHI::Resource::ResourceDesc::GetGenericBufferDesc(sizeof(SceneGlobals), sizeof(SceneGlobals)));
		meshInstancesBuffer = std::make_unique<RHI::Buffer>(device, RHI::Resource::ResourceDesc::GetGenericBufferDesc(MAX_INSTANCE_COUNT * sizeof(SceneMeshInstance), sizeof(SceneMeshInstance)));				
		meshMaterialsBuffer = std::make_unique<RHI::Buffer>(device, RHI::Resource::ResourceDesc::GetGenericBufferDesc(MAX_MATERIAL_COUNT * sizeof(SceneMaterial), sizeof(SceneMaterial)));
		lightBuffer = std::make_unique<RHI::Buffer>(device, RHI::Resource::ResourceDesc::GetGenericBufferDesc(MAX_LIGHT_COUNT * sizeof(SceneLight), sizeof(SceneLight)));
	};

	auto get_SceneGlobalsBuffer() { return globalBuffer.get(); }
	auto get_SceneMeshInstancesBuffer() { return meshInstancesBuffer.get(); }
	auto get_SceneMeshMaterialsBuffer() { return meshMaterialsBuffer.get(); }
	auto get_SceneLightBuffer() { return lightBuffer.get(); }
	auto const& get_SceneGlobals() { return stats; }
	auto const& get_SceneInstances() { return meshes; }
	auto const& get_SceneMaterials() { return materials; }
	auto const& get_SceneLights() { return lights; }
	void update(FrameData frame) {
		// updates for static mesh view	
		// xxx skinned meshes 
		meshes.clear(); materials.clear();
		for (auto& mesh : frame.scene->registry.storage<StaticMeshComponent>()) {
			auto& asset = frame.scene->assets.get<StaticMeshAsset>(mesh.mesh_resource);
			SceneMeshInstance sceneMesh{};					
			AffineTransform transform = mesh.get_global_transform();
			sceneMesh.transform = transform.Transpose();
			sceneMesh.transformInvTranspose = transform.Invert()/*.Transpose().Transpose()*/;
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
			auto pMaterial = frame.scene->try_get<MaterialComponet>(mesh.material);
			if (pMaterial) {
				SceneMaterial material;		
				auto try_set_heap_handle = [&](auto handle, auto& dest) {
					auto ptr = frame.scene->assets.try_get_base_ptr<ImageAsset, SDRImageAsset>(handle);
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
			sceneMesh.boundingBox = asset.boundingBox;
			sceneMesh.boundingSphere = asset.boundingSphere;

			sceneMesh.lodOverride = mesh.lodOverride;
			meshes.push_back(sceneMesh);
		}
		meshInstancesBuffer->Update(meshes.data(), ::size_in_bytes(meshes), 0);
		meshMaterialsBuffer->Update(materials.data(), ::size_in_bytes(materials), 0);
		// lights
		lights.clear();
		for (auto& light : frame.scene->registry.storage<LightComponent>()) {
			SceneLight sceneLight{};
			AffineTransform transform = light.get_global_transform();
			Vector3 translation = transform.Translation();
			Vector3 direction = Vector3::Transform({ 0,-1,0 }, transform.Quaternion());
			sceneLight.position.x = translation.x;
			sceneLight.position.y = translation.y;
			sceneLight.position.z = translation.z;
			sceneLight.position.w = 1;
			sceneLight.direction.x = direction.x;
			sceneLight.direction.y = direction.y;
			sceneLight.direction.z = direction.z;
			sceneLight.direction.w = 1;
			sceneLight.color = light.color;
			sceneLight.type = (UINT)light.type;
			sceneLight.intensity = light.intensity;
			sceneLight.radius = light.radius;
			lights.push_back(sceneLight);
		}
		lightBuffer->Update(lights.data(), ::size_in_bytes(lights), 0);
		// update scene globals		
		stats.numMeshInstances = meshes.size();
		stats.numLights = lights.size();
		// scene camera
		if (frame.scene->registry.any_of<CameraComponent>(frame.scene->active_camera)) {
			auto& camera = frame.scene->get<CameraComponent>(frame.scene->active_camera);
			AffineTransform transform = camera.get_global_transform();
			stats.camera = camera.get_struct(frame.viewportWidth / (float)frame.viewportHeight, transform);
		}
		else {
			LOG(ERROR) << "No active camera set!";
		}
		stats.frameIndex = frame.frameIndex;
		stats.frameDimension.x = frame.viewportWidth;
		stats.frameDimension.y = frame.viewportHeight;
		stats.frameFlags = frame.frameFlags;
		stats.backBufferIndex = frame.backBufferIndex;
		stats.sceneVersion = frame.scene->get_version();
		globalBuffer->Update(&stats, sizeof(stats), 0);
	}
};