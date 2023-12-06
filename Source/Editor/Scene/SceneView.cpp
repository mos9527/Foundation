#include "SceneView.hpp"

#include "../Processor/IBLProbeProcessor.hpp"
#include "../Processor/LTCTableProcessor.hpp"

bool SceneView::update(RHI::CommandList* ctx,Scene& scene, SceneCameraComponent& camera, FrameData&& _frameData, ShaderData&& _shaderData) {
	frameData = _frameData;
	shaderData = _shaderData;
	auto& static_meshes = scene.storage<SceneStaticMeshComponent>();
	auto& skinned_meshes = scene.storage<SceneSkinnedMeshComponent>();
	auto& materials = scene.storage<AssetMaterialComponent>();
	auto& lights = scene.storage<SceneLightComponent>();	
	std::mutex rwMutex;
	auto update_mesh = [&]<typename T>(T& mesh) {		
		{
			// std::scoped_lock lock(rwMutex);			
			if (viewedComponentVersions[mesh.get_entity()] == mesh.get_version())
				return;
		}
		uint localIndex = scene.index<T>(mesh.get_entity()); // the index in Skinned or Static mesh storage			
		// HACK: Since the static/skinned storages are seperate
		// we use the static's size as the offset for the skinned instances
		// so we can express them in one value.
		uint globalIndex = localIndex;
		if constexpr (std::is_same_v<T, SceneSkinnedMeshComponent>)
			globalIndex += static_meshes.size();
		AffineTransform transform = mesh.get_global_transform();
		// Instance Data
		SceneMeshInstanceData sceneMesh = *meshInstancesBuffer.DataAt(globalIndex);
		sceneMesh.instanceMeshGlobalIndex = globalIndex;
		sceneMesh.instanceMeshLocalIndex = localIndex;	
		// Transforms
		sceneMesh.transformPrev = sceneMesh.version == 0 ? transform.Transpose() : sceneMesh.transform;
		sceneMesh.transform = transform.Transpose();
		sceneMesh.transformInvTranspose = transform.Invert()/*.Transpose().Transpose()*/;		
		// Flags		
		sceneMesh.instanceFlags = 0;
		if (!mesh.get_enabled())
			sceneMesh.instanceFlags |= INSTANCE_FLAG_INVISIBLE;		
		if (mesh.get_selected())
			sceneMesh.instanceFlags |= INSTANCE_FLAG_SILHOUETTE;		
		// Material
		AssetMaterialComponent& materialComponent = scene.get<AssetMaterialComponent>(mesh.materialComponent);
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
		sceneMesh.instanceMaterialIndex = materials.index(mesh.materialComponent);		
		// More Flags
		if (materialComponent.has_alpha()) 
			sceneMesh.instanceFlags |= INSTANCE_FLAG_TRANSPARENCY;
		// Write the update
		{
			// std::scoped_lock lock(rwMutex);
			// Buffer Indices
			if constexpr (std::is_same_v<T, SceneStaticMeshComponent>) {
				sceneMesh.instanceMeshType = INSTANCE_MESH_TYPE_STATIC;
				staticMeshBuffers.RegisterOrUpdate(&mesh);
			}
			if constexpr (std::is_same_v<T, SceneSkinnedMeshComponent>) {
				sceneMesh.instanceMeshType = INSTANCE_MESH_TYPE_SKINNED;
				skinnedMeshBuffers.RegisterOrUpdate(ctx, &mesh);
			}
			*meshMaterialsBuffer.DataAt(sceneMesh.instanceMaterialIndex) = material;
			*meshInstancesBuffer.DataAt(sceneMesh.instanceMeshGlobalIndex) = sceneMesh;
			viewedComponentVersions[mesh.get_entity()] = mesh.get_version();
		}
	};
	// Static Meshes
	std::for_each(std::execution::seq, static_meshes.begin(), static_meshes.end(), update_mesh);
	// Skinned Meshes
	std::for_each(std::execution::seq, skinned_meshes.begin(), skinned_meshes.end(), update_mesh);
	// Lights
	std::for_each(std::execution::seq, lights.begin(), lights.end(), [&](SceneLightComponent& light) {
		{
			// std::scoped_lock lock(rwMutex);
			if (viewedComponentVersions[light.get_entity()] == light.get_version())
				return;
		}
		SceneLight sceneLight{};

		Vector3 translation = light.get_global_translation();
		Vector3 direction = light.get_direction_vector();
		sceneLight.enabled = light.get_enabled();
		sceneLight.position.x = translation.x;
		sceneLight.position.y = translation.y;
		sceneLight.position.z = translation.z;
		sceneLight.position.w = 1;
		sceneLight.direction.x = direction.x;
		sceneLight.direction.y = direction.y;
		sceneLight.direction.z = direction.z;
		sceneLight.direction.w = 1;

		sceneLight.type = (UINT)light.lightType;
		sceneLight.intensity = light.intensity;
		sceneLight.color = light.color;

		sceneLight.spot_point_radius = light.spot_point_radius;
		sceneLight.spot_size_rad = light.spot_size_rad;
		sceneLight.spot_size_blend = light.spot_size_blend;

		sceneLight.area_quad_disk_extents = light.area_quad_disk_extents;
		sceneLight.area_quad_disk_twoSided = light.area_quad_disk_twoSided;

		sceneLight.area_line_length = light.area_line_length;
		sceneLight.area_line_caps = light.area_line_caps;
		sceneLight.area_line_radius = light.area_line_radius;
		// Write the update
		{
			// std::scoped_lock lock(rwMutex);
			viewedComponentVersions[light.get_entity()] = light.get_version();
			*lightBuffer.DataAt(lights.index(light.get_entity())) = sceneLight;
		}
	});
	// These are updated every frameData...
	// LTC Table
	globalBuffer.Data()->ltcLutHeapIndex = shaderData.ltcTable->ltcSRV->descriptor.get_heap_handle();
	// Probe
	if (shaderData.probe.probe != nullptr && shaderData.probe.probe->state == IBLProbeProcessorStates::IdleWithProbe) {
		SceneIBLProbe probe{};
		probe.enabled = shaderData.probe.use;
		probe.cubemapHeapIndex = shaderData.probe.probe->cubemapSRV->descriptor.get_heap_handle();
		probe.radianceHeapIndex = shaderData.probe.probe->radianceCubeArraySRV->descriptor.get_heap_handle();
		probe.irradianceHeapIndex = shaderData.probe.probe->irridanceCubeSRV->descriptor.get_heap_handle();
		probe.lutHeapIndex = shaderData.probe.probe->lutArraySRV->descriptor.get_heap_handle();
		probe.mips = shaderData.probe.probe->numMips;
		probe.diffuseIntensity = shaderData.probe.diffuseIntensity;
		probe.specularIntensity = shaderData.probe.specularIntensity;
		probe.occlusionStrength = shaderData.probe.occlusionStrength;
		probe.skyboxLod = shaderData.probe.skyboxLod;
		probe.skyboxIntensity = shaderData.probe.skyboxIntensity;
		globalBuffer.Data()->probe = probe;
	}
	else {
		globalBuffer.Data()->probe.enabled = false;
	}
	// Silhouette
	globalBuffer.Data()->edgeThreshold = shaderData.silhouette.edgeThreshold;
	globalBuffer.Data()->edgeColor = shaderData.silhouette.edgeColor;
	// Scene info
	globalBuffer.Data()->numMeshInstances = skinned_meshes.size() + static_meshes.size();
	globalBuffer.Data()->numLights = lights.size();
	// Camera
	globalBuffer.Data()->cameraPrev = globalBuffer.Data()->camera;
	globalBuffer.Data()->camera = camera.get_struct(frameData.viewportWidth / (float)frameData.viewportHeight);	
	// Frame
	globalBuffer.Data()->frameIndex = frameData.frameIndex;
	globalBuffer.Data()->frameDimension.x = frameData.viewportWidth;
	globalBuffer.Data()->frameDimension.y = frameData.viewportHeight;
	globalBuffer.Data()->frameFlags = frameData.frameFlags;
	globalBuffer.Data()->backBufferIndex = frameData.backBufferIndex;
	globalBuffer.Data()->frameTimePrev = frameData.frameTimePrev;
	globalBuffer.Data()->sceneVersion = scene.get_version();
	return false;
}