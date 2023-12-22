#include "SceneView.hpp"
#include "../Processor/HDRIProbe.hpp"
#include "../Processor/LTCFittedLUT.hpp"
using namespace RHI;

SceneView::SceneView(Device* device) :
	skinnedMeshVertexBuffer(device),	
	meshInstancesBuffer(device, MAX_INSTANCE_COUNT, L"Upload Instances"),
	materialsBuffer(device, MAX_MATERIAL_COUNT, L"Upload Materials"),
	lightsBuffer(device, MAX_LIGHT_COUNT, L"Upload Lights"),
	constGlobalsBuffer(device, 1, L"Globals"),
	constShadingBuffer(device, 1, L"Shading")
{
	sceneMeshInstancesBuffer.first = std::make_unique<Buffer>(
		device,
		Resource::ResourceDesc::GetGenericBufferDesc(
			MAX_INSTANCE_COUNT * sizeof(SceneMeshInstanceData), sizeof(SceneMeshInstanceData),
			ResourceState::Common, ResourceHeapType::Default, ResourceFlags::None, L"Scene Instances"
		));
	sceneMeshInstancesBuffer.second = std::make_unique<ShaderResourceView>(
		sceneMeshInstancesBuffer.first.get(),
		ShaderResourceViewDesc::GetStructuredBufferDesc(0, MAX_INSTANCE_COUNT, sizeof(SceneMeshInstanceData))
	);

	sceneMaterialsBuffer.first = std::make_unique<Buffer>(
		device,
		Resource::ResourceDesc::GetGenericBufferDesc(
			MAX_MATERIAL_COUNT * sizeof(SceneMaterial), sizeof(SceneMaterial),
			ResourceState::Common, ResourceHeapType::Default, ResourceFlags::None, L"Scene Materials"
		));
	sceneMaterialsBuffer.second = std::make_unique<ShaderResourceView>(
		sceneMaterialsBuffer.first.get(),
		ShaderResourceViewDesc::GetStructuredBufferDesc(0, MAX_MATERIAL_COUNT, sizeof(SceneMaterial))
	);

	sceneLightsBuffer.first = std::make_unique<Buffer>(
		device,
		Resource::ResourceDesc::GetGenericBufferDesc(
			MAX_LIGHT_COUNT * sizeof(SceneLight), sizeof(SceneLight),
			ResourceState::Common, ResourceHeapType::Default, ResourceFlags::None, L"Scene Lights"
		));
	sceneLightsBuffer.second = std::make_unique<ShaderResourceView>(
		sceneLightsBuffer.first.get(),
		ShaderResourceViewDesc::GetStructuredBufferDesc(0, MAX_LIGHT_COUNT, sizeof(SceneLight))
	);

}
SceneMaterial DumpMaterialData(AssetMaterialComponent* materialComponent) {
	SceneMaterial material;
	Scene& scene = materialComponent->parent;
	auto try_set_heap_handle = [&](TextureAsset* ptr, auto& dest) {
		if (ptr && ptr->textureSRV)
			dest = ptr->textureSRV->allocate_transient_descriptor(CommandListType::Direct).get_heap_handle();
		else
			dest = INVALID_HEAP_HANDLE;
	};
	try_set_heap_handle(scene.try_get<TextureAsset>(materialComponent->albedoImage), material.albedoMap);
	try_set_heap_handle(scene.try_get<TextureAsset>(materialComponent->normalMapImage), material.normalMap);
	try_set_heap_handle(scene.try_get<TextureAsset>(materialComponent->pbrMapImage), material.pbrMap);
	try_set_heap_handle(scene.try_get<TextureAsset>(materialComponent->emissiveMapImage), material.emissiveMap);
	material.albedo = materialComponent->albedo;
	material.pbr = materialComponent->pbr;
	material.emissive = materialComponent->emissive;
	return material;
}
template<typename T> SceneMeshInstanceData DumpMeshInstanceData(MeshSkinning* meshSkinningBuffer, SceneMeshInstanceData* data, T* mesh, uint meshIndex) {
	AffineTransform transform = mesh->get_global_transform();
	Scene& scene = mesh->parent;
	// Instance Data
	SceneMeshInstanceData sceneMesh;
	sceneMesh.meshIndex = meshIndex;
	// Transforms
	sceneMesh.transformPrev = data->transform; // Copies previous transform data
	sceneMesh.transform = transform.Transpose();
	sceneMesh.transformInvTranspose = transform.Invert()/*.Transpose().Transpose()*/;
	// Materials
	sceneMesh.materialIndex = scene.index<AssetMaterialComponent>(mesh->materialComponent);
	// Flags
	AssetMaterialComponent& materialComponent = scene.get<AssetMaterialComponent>(mesh->materialComponent);
	sceneMesh.instanceFlags = 0u;
	if (mesh->get_enabled())
		sceneMesh.instanceFlags |= INSTANCE_FLAG_ENABLED;
	if (mesh->get_selected())
		sceneMesh.instanceFlags |= INSTANCE_FLAG_SILHOUETTE;
	if (materialComponent.has_alpha())
		sceneMesh.instanceFlags |= INSTANCE_FLAG_TRANSPARENT;
	else
		sceneMesh.instanceFlags |= INSTANCE_FLAG_OPAQUE;	
	// MeshBuffers (VB/LOD)
	const auto write_lod = [&](auto& asset) {
		for (int i = 0; i < MAX_LOD_COUNT; i++) {
			sceneMesh.meshBuffer.LOD[i].indices = D3D12_INDEX_BUFFER_VIEW{
				.BufferLocation = asset.lodBuffers[i]->indexBuffer.GetGPUAddress(),
				.SizeInBytes = (UINT)asset.lodBuffers[i]->indexBuffer.GetDesc().sizeInBytes(),
				.Format = DXGI_FORMAT_R32_UINT
			};
			sceneMesh.meshBuffer.LOD[i].numIndices = asset.lodBuffers[i]->numIndices;
#ifdef RHI_USE_MESH_SHADER
			sceneMesh.meshBuffer.LOD[i].meshlets = asset.lodBuffers[i]->meshletBuffer.GetGPUAddress();
			sceneMesh.meshBuffer.LOD[i].meshletVertices = asset.lodBuffers[i]->meshletVertexBuffer.GetGPUAddress();
			sceneMesh.meshBuffer.LOD[i].meshletTriangles = asset.lodBuffers[i]->meshletTriangleBuffer.GetGPUAddress();
			sceneMesh.meshBuffer.LOD[i].numMeshlets = asset.lodBuffers[i]->numMeshlets;
#endif
		}
	};
	if constexpr (std::is_same_v<T, SceneStaticMeshComponent>) {
		StaticMeshAsset& asset = mesh->parent.get<StaticMeshAsset>(mesh->meshAsset);
		sceneMesh.meshBuffer.boundingBox = asset.boundingBox;
		sceneMesh.meshBuffer.VB = D3D12_VERTEX_BUFFER_VIEW{
			.BufferLocation = asset.vertexBuffer->buffer.GetGPUAddress(),
			.SizeInBytes = (UINT)asset.vertexBuffer->buffer.GetDesc().sizeInBytes(),
			.StrideInBytes = (UINT)asset.vertexBuffer->buffer.GetDesc().stride
		};
		write_lod(asset);
	}
	if constexpr (std::is_same_v<T, SceneSkinnedMeshComponent>) {
		SkinnedMeshAsset& asset = mesh->parent.get<SkinnedMeshAsset>(mesh->meshAsset);
		auto skinnedBuffer = meshSkinningBuffer->GetSkinnedVertexBuffer(mesh);
		sceneMesh.meshBuffer.boundingBox = BoundingBox({ 0,0,0 }, { 1e10, 1e10, 1e10 });
		// xxx Recalculation is required every frame for skinned meshes (if needed)
		sceneMesh.meshBuffer.VB = D3D12_VERTEX_BUFFER_VIEW{
			.BufferLocation = skinnedBuffer.first->GetGPUAddress(),
			.SizeInBytes = (UINT)skinnedBuffer.first->GetDesc().sizeInBytes(),
			.StrideInBytes = (UINT)skinnedBuffer.first->GetDesc().stride
		};
		write_lod(asset);
	}
	return sceneMesh;
}
SceneLight DumpLightData(SceneLightComponent* light) {
	SceneLight sceneLight{};
	Vector3 translation = light->get_global_translation();
	Vector3 direction = light->get_direction_vector();
	sceneLight.enabled = light->get_enabled();
	sceneLight.position.x = translation.x;
	sceneLight.position.y = translation.y;
	sceneLight.position.z = translation.z;
	sceneLight.direction.x = direction.x;
	sceneLight.direction.y = direction.y;

	sceneLight.type = (UINT)light->lightType;
	sceneLight.intensity = light->intensity;
	sceneLight.color = light->color;

	sceneLight.spot_point_radius = light->spot_point_radius;
	sceneLight.spot_size_rad = light->spot_size_rad;
	sceneLight.spot_size_blend = light->spot_size_blend;

	sceneLight.area_quad_disk_extents = light->area_quad_disk_extents;
	sceneLight.area_quad_disk_twoSided = light->area_quad_disk_twoSided;

	sceneLight.area_line_length = light->area_line_length;
	sceneLight.area_line_caps = light->area_line_caps;
	sceneLight.area_line_radius = light->area_line_radius;
	return sceneLight;
}
void SceneView::Update(RHI::CommandList* ctx, RHIContext* rhiCtx, SceneContext* sceneCtx, EditorContext* editorCtx) {
	ZoneScopedN("Scene View Update");
	Scene* scene = sceneCtx->scene;
	CHECK(ctx->GetType() == CommandListType::Direct && "Cannot run Update on anything but Direct command list/queue!");
	auto& static_meshes = scene->storage<SceneStaticMeshComponent>();
	auto& skinned_meshes = scene->storage<SceneSkinnedMeshComponent>();
	auto& materials = scene->storage<AssetMaterialComponent>();
	auto& lights = scene->storage<SceneLightComponent>();
	// Counters
	uint numMeshInstances = skinned_meshes.size() + static_meshes.size();
	uint numMaterials = materials.size();
	uint numLights = lights.size();
	// Materials
	const auto update_material = [&](AssetMaterialComponent& material) {
		auto index = materials.index(material.get_entity());
		SceneMaterial data = DumpMaterialData(&material);
		memcpy(materialsBuffer.DataAt(index), &data, sizeof(data));
	};
	std::for_each(materials.begin(), materials.end(), update_material);
	// Lights
	const auto update_light = [&](SceneLightComponent& light) {
		if (!ValidateVersion(light)) return;
		auto index = lights.index(light.get_entity());
		SceneLight data = DumpLightData(&light);
		memcpy(lightsBuffer.DataAt(index), &data, sizeof(data));
	};
	std::for_each(lights.begin(), lights.end(), update_light);
	// Mesh Skinning
	if (skinned_meshes.size()) {
		skinnedMeshVertexBuffer.Begin(ctx);
		std::for_each(skinned_meshes.begin(), skinned_meshes.end(), [&](SceneSkinnedMeshComponent& mesh) {
			if (!ValidateVersion(mesh)) return;
			skinnedMeshVertexBuffer.Update(ctx, &mesh);
			});
		skinnedMeshVertexBuffer.End(ctx);
	}
	// Meshes
	const auto update_mesh = [&](auto& mesh) -> void {
		if (!ValidateVersion(mesh)) return;
		auto index = EncodeMeshInstanceIndex(&mesh);
		SceneMeshInstanceData* instance = meshInstancesBuffer.DataAt(index);
		SceneMeshInstanceData data = DumpMeshInstanceData(&skinnedMeshVertexBuffer, instance, &mesh, index);
		memcpy(instance, &data, sizeof(data));
	};
	std::for_each(static_meshes.begin(), static_meshes.end(), update_mesh);
	std::for_each(skinned_meshes.begin(), skinned_meshes.end(), update_mesh);
	// Versions
	const auto update_version = [&](auto& component) -> void { UpdateVersion(component); };
	std::for_each(lights.begin(), lights.end(), update_version);
	std::for_each(static_meshes.begin(), static_meshes.end(), update_version);
	std::for_each(skinned_meshes.begin(), skinned_meshes.end(), update_version);
	// Update Glboal Constants
	// Runtime info
	constGlobalsBuffer.Data()->sceneVersion = scene->get_version();
	constGlobalsBuffer.Data()->backBufferIndex = rhiCtx->swapchain->GetCurrentBackbufferIndex();
	constGlobalsBuffer.Data()->frameIndex = rhiCtx->swapchain->GetFrameIndex();
	constGlobalsBuffer.Data()->frameTime = rhiCtx->swapchain->GetPrevFrametime();
	constGlobalsBuffer.Data()->renderDimension = { editorCtx->render.width, editorCtx->render.height };
	constGlobalsBuffer.Data()->viewportDimension = { editorCtx->viewport.width, editorCtx->viewport.height };
	// Camera
	float aspect = editorCtx->viewport.width / (float)editorCtx->viewport.height;
	auto& activeCamera = scene->get<SceneCameraComponent>(editorCtx->activeCamera);
	constGlobalsBuffer.Data()->cameraPrev = constGlobalsBuffer.Data()->camera;
	constGlobalsBuffer.Data()->camera = activeCamera.get_hlsl(aspect);
	// Shading Constants
	// HDRI
	SceneIBLProbe probeHLSL{};
	{
		auto* probeComponent = sceneCtx->scene->try_get<AssetHDRIProbeComponent>(editorCtx->editorHDRI);
		if (probeComponent && probeComponent->get_probe()->state == HDRIProbeProcessorStates::IdleWithProbe) {
			auto probe = probeComponent->get_probe();
			probeHLSL.enabled = true;
			probeHLSL.cubemapHeapIndex = probe->cubemapSRV->allocate_transient_descriptor(ctx).get_heap_handle();
			probeHLSL.radianceHeapIndex = probe->radianceCubeArraySRV->allocate_transient_descriptor(ctx).get_heap_handle();
			probeHLSL.irradianceHeapIndex = probe->irridanceCubeSRV->allocate_transient_descriptor(ctx).get_heap_handle();
			probeHLSL.lutHeapIndex = probe->lutArraySRV->allocate_transient_descriptor(ctx).get_heap_handle();
			probeHLSL.mips = probe->numMips;
			probeHLSL.diffuseIntensity = editorCtx->iblProbe.diffuseIntensity;
			probeHLSL.specularIntensity = editorCtx->iblProbe.specularIntensity;
			probeHLSL.occlusionStrength = editorCtx->iblProbe.occlusionStrength;
		}
		else {
			probeHLSL.enabled = false;
		}
	}
	constShadingBuffer.Data()->iblProbe = probeHLSL;
	constShadingBuffer.Data()->ltcLut = rhiCtx->data_LTCLUT->ltcSRV->allocate_transient_descriptor(ctx).get_heap_handle();
	// Buffer Metadatas
	constGlobalsBuffer.Data()->meshInstances = {
		.heapIndex = sceneMeshInstancesBuffer.second->allocate_transient_descriptor(ctx).get_heap_handle(),
		.count = numMeshInstances
	};	
	constGlobalsBuffer.Data()->materials = {
		.heapIndex = sceneMaterialsBuffer.second->allocate_transient_descriptor(ctx).get_heap_handle(),
		.count = numMaterials
	};	
	constGlobalsBuffer.Data()->lights = {
		.heapIndex = sceneLightsBuffer.second->allocate_transient_descriptor(ctx).get_heap_handle(),
		.count = numLights
	};
	// Upload to GPU
	ctx->QueueTransitionBarrier(sceneMeshInstancesBuffer.first.get(), ResourceState::CopyDest);
	ctx->QueueTransitionBarrier(sceneMaterialsBuffer.first.get(), ResourceState::CopyDest);
	ctx->QueueTransitionBarrier(sceneLightsBuffer.first.get(), ResourceState::CopyDest);
	ctx->FlushBarriers();
	ctx->CopyBufferRegion(&meshInstancesBuffer, sceneMeshInstancesBuffer.first.get(), 0, 0, numMeshInstances * sizeof(SceneMeshInstanceData));
	ctx->CopyBufferRegion(&materialsBuffer, sceneMaterialsBuffer.first.get(), 0, 0, numMaterials * sizeof(SceneMaterial));
	ctx->CopyBufferRegion(&lightsBuffer, sceneLightsBuffer.first.get(), 0, 0, numLights * sizeof(SceneLight));
}
