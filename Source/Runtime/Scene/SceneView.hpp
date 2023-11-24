#pragma once
#include "../RHI/RHI.hpp"
#include "../Asset/ResourceContainer.hpp"
#include "../../Runtime/Renderer/Shaders/Shared.h"

#include "Scene.hpp"
#include "SceneComponent/Mesh.hpp"
#include "SceneComponent/Camera.hpp"
#include "SceneComponent/Light.hpp"
#include "AssetComponet/Material.hpp"
#include "AssetComponet/Mesh.hpp"

class SceneView {
public:
private:
	BufferContainer<SceneMeshInstance> meshInstancesBuffer;
	BufferContainer<SceneMaterial> meshMaterialsBuffer;
	BufferContainer<SceneLight> lightBuffer;	
	BufferContainer<SceneGlobals> globalBuffer;

	std::unordered_map<entt::entity, size_t> scenecomponent_versions;
	size_t current_scene_version = 0;
public:
	struct FrameData {
		uint viewportWidth;
		uint viewportHeight;
		uint frameIndex;
		uint frameFlags;
		uint backBufferIndex;
		float frameTimePrev;
	};
	SceneView(const SceneView&) = delete;
	SceneView(SceneView&&) = default;
	SceneView(RHI::Device* device) :
		meshInstancesBuffer(device, MAX_INSTANCE_COUNT),
		meshMaterialsBuffer(device, MAX_MATERIAL_COUNT),
		lightBuffer(device,MAX_LIGHT_COUNT),
		globalBuffer(device, 1)	
	{
		globalBuffer.Data()->frameFlags = FRAME_FLAG_DEFAULT;
	};
	SceneGlobals const& get_SceneGlobals() { return *globalBuffer.Data(); }
	RHI::Resource* get_SceneGlobalsBuffer() { return &globalBuffer; }
	RHI::Resource* get_SceneMeshInstancesBuffer() { return &meshInstancesBuffer; }
	RHI::Resource* get_SceneMeshMaterialsBuffer() { return &meshMaterialsBuffer; }
	RHI::Resource* get_SceneLightBuffer() { return &lightBuffer; }
	
	void update(Scene& scene, SceneCameraComponent& camera, FrameData&& frame) {
		// Only update scene when it's updated
		if (scene.get_version() != current_scene_version) {
			auto& instances = scene.storage<SceneMeshComponent>();
			auto& materials = scene.storage<AssetMaterialComponent>();
			auto& lights = scene.storage<SceneLightComponent>();
			for (auto& mesh : instances) {
				// Only update instances that got updated
				if (mesh.get_version() == scenecomponent_versions[mesh.get_entity()]) continue;
				scenecomponent_versions[mesh.get_entity()] = mesh.get_version();				
				// SceneMeshComponent -> AssetMeshComponent -> MeshAsset
				AssetMeshComponent& assetComponent = scene.get<AssetMeshComponent>(mesh.meshAsset);
				MeshAsset& asset = scene.get<MeshAsset>(assetComponent.mesh);
				// Create HLSL data for update
				SceneMeshInstance sceneMesh{};
				AffineTransform transform = mesh.get_global_transform();
				sceneMesh.transformPrev = sceneMesh.transform;
				sceneMesh.transformInvTransposePrev = sceneMesh.transformInvTransposePrev;
				sceneMesh.transform = transform.Transpose();
				sceneMesh.transformInvTranspose = transform.Invert()/*.Transpose().Transpose()*/;
				sceneMesh.vertices = D3D12_VERTEX_BUFFER_VIEW{
					.BufferLocation = asset.vertexBuffer->buffer.GetGPUAddress(),
					.SizeInBytes = (UINT)asset.vertexBuffer->buffer.GetDesc().sizeInBytes(),
					.StrideInBytes = (UINT)asset.vertexBuffer->buffer.GetDesc().stride
				};
				sceneMesh.numVertices = asset.vertexBuffer->numVertices;
				for (int i = 0; i < MAX_LOD_COUNT; i++) {
					sceneMesh.lods[i].indices = D3D12_INDEX_BUFFER_VIEW{
						.BufferLocation = asset.lodBuffers[i]->indexBuffer.GetGPUAddress(),
						.SizeInBytes = (UINT)asset.lodBuffers[i]->indexBuffer.GetDesc().sizeInBytes(),
						.Format = DXGI_FORMAT_R32_UINT
					};
					sceneMesh.lods[i].meshlets = asset.lodBuffers[i]->meshletBuffer.GetGPUAddress();
					sceneMesh.lods[i].meshletVertices = asset.lodBuffers[i]->meshletVertexBuffer.GetGPUAddress();
					sceneMesh.lods[i].meshletTriangles = asset.lodBuffers[i]->meshletTriangleBuffer.GetGPUAddress();
					sceneMesh.lods[i].numIndices = asset.lodBuffers[i]->numIndices;
					sceneMesh.lods[i].numMeshlets = asset.lodBuffers[i]->numMeshlets;
				}
				// SceneMeshComponent -> AssetMeshComponent -> MeshAsset
				AssetMaterialComponent& materialComponent = scene.get<AssetMaterialComponent>(mesh.materialAsset);
				SceneMaterial material;
				auto try_set_heap_handle = [&](TextureAsset* ptr, auto& dest) {
					if (ptr && ptr->textureSRV)
						dest = ptr->textureSRV->descriptor.get_heap_handle();
					else
						dest = INVALID_HEAP_HANDLE;
				};
				try_set_heap_handle(scene.try_get<TextureAsset>(materialComponent.albedoImage), material.albedoMap);
				try_set_heap_handle(scene.try_get<TextureAsset>(materialComponent.normalMapImage), material.normalMap);
				try_set_heap_handle(scene.try_get<TextureAsset>(materialComponent.pbrMapImage), material.pbrMap);
				try_set_heap_handle(scene.try_get<TextureAsset>(materialComponent.emissiveMapImage), material.emissiveMap);
				material.albedo = materialComponent.albedo;
				material.pbr = materialComponent.pbr;
				material.emissive = materialComponent.emissive;
				sceneMesh.materialIndex = materials.index(mesh.materialAsset);
				// Write the update
				*meshMaterialsBuffer.DataAt(sceneMesh.materialIndex) = material;
				// More data with the mesh
				sceneMesh.instanceIndex = instances.index(mesh.get_entity());
				sceneMesh.boundingBox = asset.boundingBox;
				sceneMesh.boundingSphere = asset.boundingSphere;
				sceneMesh.lodOverride = mesh.lodOverride;
				sceneMesh.instanceFlags = 0;
				if (mesh.isOccludee) sceneMesh.instanceFlags |= INSTANCE_FLAG_OCCLUDEE;
				if (mesh.visible) sceneMesh.instanceFlags |= INSTANCE_FLAG_VISIBLE;
				if (mesh.drawBoundingBox) sceneMesh.instanceFlags |= INSTANCE_FLAG_DRAW_BOUNDS;
				// Write the update
				*meshInstancesBuffer.DataAt(sceneMesh.instanceIndex) = sceneMesh;
				LOG(INFO) << "Update MESH ver " << mesh.get_version() << " of " << sceneMesh.instanceIndex;
			}
			// lights			
			for (auto& light : lights) {
				if (scenecomponent_versions[light.get_entity()] == light.get_version()) continue;
				scenecomponent_versions[light.get_entity()] = light.get_version();
				LOG(INFO) << "Update LIGHT ver " << light.get_version() << " of " << light.get_name();
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
				sceneLight.type = (UINT)light.lightType;
				sceneLight.intensity = light.intensity;
				sceneLight.radius = light.radius;
				*lightBuffer.DataAt(lights.index(light.get_entity())) = sceneLight;
			}
			// globals
			globalBuffer.Data()->numMeshInstances = instances.size();
			globalBuffer.Data()->numLights = lights.size();				
		}
		// Cameras are updated every frame
		globalBuffer.Data()->cameraPrev = globalBuffer.Data()->camera;
		globalBuffer.Data()->camera = camera.get_struct(frame.viewportWidth / (float)frame.viewportHeight);
		
		globalBuffer.Data()->frameIndex = frame.frameIndex;
		globalBuffer.Data()->frameDimension.x = frame.viewportWidth;
		globalBuffer.Data()->frameDimension.y = frame.viewportHeight;
		globalBuffer.Data()->frameFlags = frame.frameFlags;
		globalBuffer.Data()->backBufferIndex = frame.backBufferIndex;
		globalBuffer.Data()->frameTimePrev = frame.frameTimePrev;
		globalBuffer.Data()->sceneVersion = scene.version;
		current_scene_version = scene.get_version();
	}
};