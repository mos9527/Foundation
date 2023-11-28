#include "SceneView.hpp"

#include "../Processor/IBLProbeProcessor.hpp"
bool SceneView::update(Scene& scene, SceneCameraComponent& camera, FrameData&& frame, ShaderData&& shader) {
	auto& instances = scene.storage<SceneMeshComponent>();
	auto& materials = scene.storage<AssetMaterialComponent>();
	auto& lights = scene.storage<SceneLightComponent>();
	std::mutex write_mutex;
	// Instances
	std::for_each(std::execution::seq, instances.begin(), instances.end(), [&](SceneMeshComponent& mesh) {
		// Only update instances that got updated
		if (mesh.get_version() == scenecomponent_versions[mesh.get_entity()]) return;			
		// SceneMeshComponent -> AssetMeshComponent -> MeshAsset
		AssetMeshComponent& assetComponent = scene.get<AssetMeshComponent>(mesh.meshAsset);
		MeshAsset& asset = scene.get<MeshAsset>(assetComponent.mesh);
		// Create HLSL data for update
		uint meshIndex = instances.index(mesh.get_entity());
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
			sceneMesh.lods[i].numIndices = asset.lodBuffers[i]->numIndices;
#ifdef RHI_USE_MESH_SHADER
			sceneMesh.lods[i].meshlets = asset.lodBuffers[i]->meshletBuffer.GetGPUAddress();
			sceneMesh.lods[i].meshletVertices = asset.lodBuffers[i]->meshletVertexBuffer.GetGPUAddress();
			sceneMesh.lods[i].meshletTriangles = asset.lodBuffers[i]->meshletTriangleBuffer.GetGPUAddress();
			sceneMesh.lods[i].numMeshlets = asset.lodBuffers[i]->numMeshlets;
#endif
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
			if (materialComponent.has_alpha()) sceneMesh.instanceFlags |= INSTANCE_FLAG_TRANSPARENCY;
			if (mesh.isOccludee)
				sceneMesh.instanceFlags |= INSTANCE_FLAG_OCCLUDEE;
			if (!mesh.get_enabled())
				sceneMesh.instanceFlags |= INSTANCE_FLAG_INVISIBLE;
			// Write the update
			std::scoped_lock write_lock(write_mutex);
			scenecomponent_versions[mesh.get_entity()] = mesh.get_version();
			*meshInstancesBuffer.DataAt(sceneMesh.instanceIndex) = sceneMesh;
		});
	// Lights
	std::for_each(std::execution::par, lights.begin(), lights.end(), [&](SceneLightComponent& light) {
		if (scenecomponent_versions[light.get_entity()] == light.get_version()) return;
		SceneLight sceneLight{};
		AffineTransform transform = light.get_global_transform();
		Vector3 translation = transform.Translation();
		Vector3 direction = Vector3::Transform({ 0,-1,0 }, transform.Quaternion());
		sceneLight.enabled = light.get_enabled();
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
		// Write the update
		std::scoped_lock write_lock(write_mutex);
		scenecomponent_versions[light.get_entity()] = light.get_version();
		*lightBuffer.DataAt(lights.index(light.get_entity())) = sceneLight;
	});
	// These are updated every frame...
	// Probe
	if (shader.probe.probe != nullptr && shader.probe.probe->state == IBLProbeProcessorStates::IdleWithProbe) {
		SceneIBLProbe probe{};
		probe.enabled = shader.probe.use;
		probe.cubemapHeapIndex = shader.probe.probe->cubemapSRV->descriptor.get_heap_handle();
		probe.radianceHeapIndex = shader.probe.probe->radianceCubeArraySRV->descriptor.get_heap_handle();
		probe.irradianceHeapIndex = shader.probe.probe->irridanceCubeSRV->descriptor.get_heap_handle();
		probe.lutHeapIndex = shader.probe.probe->lutArraySRV->descriptor.get_heap_handle();
		probe.mips = shader.probe.probe->numMips;
		probe.diffuseIntensity = shader.probe.diffuseIntensity;
		probe.specularIntensity = shader.probe.specularIntensity;
		probe.occlusionStrength = shader.probe.occlusionStrength;
		globalBuffer.Data()->probe = probe;
	}
	else {
		globalBuffer.Data()->probe.enabled = false;
	}
	// Scene info
	globalBuffer.Data()->numMeshInstances = instances.size();
	globalBuffer.Data()->numLights = lights.size();
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
	return false;
}