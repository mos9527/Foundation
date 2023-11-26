#include "SceneView.hpp"
bool SceneView::update(Scene& scene, SceneCameraComponent& camera, FrameData&& frame) {
	uint meshInstanceCount = 0, meshTransparencyInstanceCount = 0;	
	uint numLights = 0;
	auto& materials = scene.storage<AssetMaterialComponent>();
	auto update_mesh_instance = [&](SceneMeshComponent& mesh) {
		// SceneMeshComponent -> AssetMeshComponent -> MeshAsset
		AssetMeshComponent& assetComponent = scene.get<AssetMeshComponent>(mesh.meshAsset);
		MeshAsset& asset = scene.get<MeshAsset>(assetComponent.mesh);
		// Create HLSL data for update
		uint meshIndex = meshInstanceCount++;
		SceneMeshInstance sceneMesh = *meshInstancesBuffer.DataAt(meshIndex);
		AffineTransform transform = mesh.get_global_transform();
		sceneMesh.transformPrev = sceneMesh.version == 0 ? transform.Transpose() : sceneMesh.transform;
		sceneMesh.version = mesh.get_version();
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
		sceneMesh.instanceIndex = meshIndex;
		sceneMesh.boundingBox = asset.boundingBox;
		sceneMesh.boundingSphere = asset.boundingSphere;
		sceneMesh.lodOverride = mesh.lodOverride;
		sceneMesh.instanceFlags = 0;
		if (materialComponent.has_alpha()) {
			sceneMesh.instanceFlags |= INSTANCE_FLAG_TRANSPARENCY;
			meshTransparencyInstanceCount++;
		}
		if (mesh.isOccludee) sceneMesh.instanceFlags |= INSTANCE_FLAG_OCCLUDEE;		
		
		// Write the update
		*meshInstancesBuffer.DataAt(sceneMesh.instanceIndex) = sceneMesh;
	};
	auto update_light = [&](SceneLightComponent& light) {
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
		*lightBuffer.DataAt(numLights++) = sceneLight;
	};
	// Only update scene when it's updated
	if (scene.get_version() != current_scene_version) {
		auto dfs_nodes = [&](auto& func, entt::entity entity, uint depth) -> void {
			SceneComponentType type = scene.get_type<SceneComponentType>(entity);
			SceneComponent* componet = scene.get_base<SceneComponent>(entity);
			if (!componet->enabled) return;
			switch (type)
			{
			case SceneComponentType::Mesh:
				update_mesh_instance(*static_cast<SceneMeshComponent*>(componet));
				break;
			case SceneComponentType::Light:
				update_light(*static_cast<SceneLightComponent*>(componet));
				break;
			case SceneComponentType::Unknown:			
			case SceneComponentType::Collection:				
			case SceneComponentType::Camera:				
			default:
				break;
			}
			if (scene.graph->has_child(entity))
				for (auto child : scene.graph->child_of(entity))
					func(func, child, depth + 1);
		};
		dfs_nodes(dfs_nodes, scene.graph->get_root(), 0);
		globalBuffer.Data()->numMeshInstances = meshInstanceCount;
		globalBuffer.Data()->numTransparencyMeshInstances = meshTransparencyInstanceCount;
		globalBuffer.Data()->numLights = numLights;
	}
	// These are updated every frame...	
	// Camera
	globalBuffer.Data()->cameraPrev = globalBuffer.Data()->camera;
	globalBuffer.Data()->camera = camera.get_struct(frame.viewportWidth / (float)frame.viewportHeight);
	// Frame
	globalBuffer.Data()->frameIndex = frame.frameIndex;
	globalBuffer.Data()->frameDimension.x = frame.viewportWidth;
	globalBuffer.Data()->frameDimension.y = frame.viewportHeight;
	globalBuffer.Data()->frameFlags = frame.frameFlags;
	globalBuffer.Data()->backBufferIndex = frame.backBufferIndex;
	globalBuffer.Data()->frameTimePrev = frame.frameTimePrev;
	globalBuffer.Data()->sceneVersion = scene.get_version();
	if (scene.get_version() != current_scene_version) {
		current_scene_version = scene.get_version();
		LOG(INFO) << "Updated view " << frame.backBufferIndex << " with scene version " << scene.version;
		LOG(INFO) << "No. Instances: " << meshInstanceCount;
		return true;
	}
	return false;
}