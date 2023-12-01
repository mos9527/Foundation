#include "Deferred.hpp"
using namespace RHI;
/*
* Deferred Rendering Pipeline
* :: GBuffer Layout ::
* ┌──────────────────┬────────────┬──────────┬───────────────────┐
* │ RT0    Albedo R  │  Albedo G  │ Abledo B │  Ambient Occlusion│ [R8G8B8A8UNROM]
* │ RT1       World.Normal        │        World.Normal          │ [R16G16FLOAT]
* │ RT2     Roughness│  Metallic  │      Instance Index          │ [R8G8B8A8UNORM]
* │ RT3    Emissive R│ Emissive G │Emissive B│         -----     │ [R8G8B8A8UNORM]
* │ RT4        Velocity           │        Velocity              │ [R16G16FLOAT]
* └───────────────────────────────┴──────────────────────────────┘
* <----------------------------32 Bits--------------------------->
* [ FB ] RGB16 Linear HDR Image
* :: Note on Transparency ::
* - Weight-Blended order-independent transparency is implemented for order stochastic transparency
* - However, if WBOIT is not used, the reset of the pipeline can still handle material parts with complete transparency (alpha=0) by
*   discarding those pixels at Gbuffer generation.
*/
void DeferredRenderer::Render(SceneView* sceneView)
{
	UINT width = sceneView->get_SceneGlobals().frameDimension.x, height = sceneView->get_SceneGlobals().frameDimension.y;
	RenderGraph rg(cache);	
	/* Resources */
	// Opaque / GBuffer
	auto& indirectCmds = rg.create<Buffer>(pass_IndirectCull.GetCountedIndirectCmdBufferDesc(L"Opaque GBuffer CMD"));
	auto& indirectCmdsUAV = rg.create<UnorderedAccessView>(pass_IndirectCull.GetCountedIndirectCmdBufferUAVDesc(indirectCmds));
	// WBOIT
	auto& transparencyIndirectCmds = rg.create<Buffer>(pass_IndirectCull.GetCountedIndirectCmdBufferDesc(L"Transparency CMD"));
	auto& transparencyIndirectCmdsUAV = rg.create<UnorderedAccessView>(pass_IndirectCull.GetCountedIndirectCmdBufferUAVDesc(transparencyIndirectCmds));
	// Silhouette
	auto& silhouetteIndirectCmds = rg.create<Buffer>(pass_IndirectCull.GetCountedIndirectCmdBufferDesc(L"Silhouette CMD"));
	auto& silhouetteIndirectCmdsUAV = rg.create<UnorderedAccessView>(pass_IndirectCull.GetCountedIndirectCmdBufferUAVDesc(silhouetteIndirectCmds));
	// Helper buffer for Culling
	auto& instanceVisibility = rg.create<Buffer>(Resource::ResourceDesc::GetGenericBufferDesc(
		MAX_INSTANCE_COUNT, RAW_BUFFER_STRIDE, ResourceState::CopyDest, ResourceHeapType::Default,
		ResourceFlags::UnorderedAccess, L"Instance Visibilties"
	));
	auto& albedo = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R8G8B8A8_UNORM, ResourceDimension::Texture2D,
		width, height, 1, 1, 1, 0,
		ResourceFlags::RenderTarget, ResourceHeapType::Default,
		ResourceState::RenderTarget, ClearValue(0, 0, 0, 0),
		L"GBuffer Albedo"
	));	
	auto& normal = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R16G16_FLOAT, ResourceDimension::Texture2D,
		width, height, 1, 1, 1, 0,
		ResourceFlags::RenderTarget, ResourceHeapType::Default,
		ResourceState::RenderTarget, ClearValue(0, 0, 0, 0),
		L"Normal Buffer"
	));
	auto& velocity = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R16G16_FLOAT, ResourceDimension::Texture2D,
		width, height, 1, 1, 1, 0,
		ResourceFlags::RenderTarget, ResourceHeapType::Default,
		ResourceState::RenderTarget, ClearValue(0, 0, 0, 0),
		L"Veloctiy Buffer"
	));
	auto& material = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R8G8B8A8_UNORM, ResourceDimension::Texture2D,
		width, height, 1, 1, 1, 0,
		ResourceFlags::RenderTarget, ResourceHeapType::Default,
		ResourceState::RenderTarget, ClearValue(0, 0, 0, 0),
		L"Material Buffer"
	));
	auto& emissive = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R8G8B8A8_UNORM, ResourceDimension::Texture2D,
		width, height, 1, 1, 1, 0,
		ResourceFlags::RenderTarget, ResourceHeapType::Default,
		ResourceState::RenderTarget, ClearValue(0, 0, 0, 0),
		L"Emissive Buffer"
	));
	auto& depth = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::D32_FLOAT, ResourceDimension::Texture2D,
		width, height, 1, 1, 1, 0,
		ResourceFlags::DepthStencil, ResourceHeapType::Default,
		ResourceState::DepthWrite,
#ifdef INVERSE_Z			
		ClearValue(0.0f, 0),
#else
		ClearValue(1.0f, 0),
#endif
		L"GBuffer Depth"
	));	
	auto& depth_dsv = rg.create<DepthStencilView>({
		.viewDesc = DepthStencilViewDesc::GetTexture2DDepthBufferDesc(ResourceFormat::D32_FLOAT, 0),
		.viewed = depth
	});
	auto& depth_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R32_FLOAT, 0, 1),
		.viewed = depth
	});
	auto& silhouetteDepth = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::D32_FLOAT, ResourceDimension::Texture2D,
		width, height, 1, 1, 1, 0,
		ResourceFlags::DepthStencil, ResourceHeapType::Default,
		ResourceState::DepthWrite,
#ifdef INVERSE_Z			
		ClearValue(0.0f, 0),
#else
		ClearValue(1.0f, 0),
#endif
		L"Silhouette Depth"
	));
	auto& silhouetteDepth_dsv = rg.create<DepthStencilView>({
		.viewDesc = DepthStencilViewDesc::GetTexture2DDepthBufferDesc(ResourceFormat::D32_FLOAT, 0),
		.viewed = silhouetteDepth
	});
	auto& silhouetteDepth_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R32_FLOAT, 0, 1),
		.viewed = silhouetteDepth
	});
	auto& albedo_rtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DDesc(ResourceFormat::R8G8B8A8_UNORM, 0),
		.viewed = albedo
	});
	auto& normal_rtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DDesc(ResourceFormat::R16G16_FLOAT, 0),
		.viewed = normal
	});
	auto& material_rtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DDesc(ResourceFormat::R8G8B8A8_UNORM, 0),
		.viewed = material
	});
	auto& emissive_rtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DDesc(ResourceFormat::R8G8B8A8_UNORM, 0),
		.viewed = emissive
	});
	auto& velocity_rtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DDesc(ResourceFormat::R16G16_FLOAT, 0),
		.viewed = velocity
	});
	auto& hiz_buffer = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R32_FLOAT, ResourceDimension::Texture2D,
		width, height, Resource::ResourceDesc::numMipsOfDimension(width, height), 1, 1, 0,
		ResourceFlags::UnorderedAccess, ResourceHeapType::Default,
		ResourceState::UnorderedAccess, {}, L"Hierarchal Z Buffer"
	));
	auto& accumalation_buffer = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R16G16B16A16_FLOAT, ResourceDimension::Texture2D,
		width, height, 1, 1, 1, 0,
		ResourceFlags::RenderTarget, ResourceHeapType::Default,
		ResourceState::RenderTarget, ClearValue(0, 0, 0, 0),
		L"Transparency Accumalation Buffer"
	));
	auto& revealage_buffer = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R16_FLOAT, ResourceDimension::Texture2D,
		width, height, 1, 1, 1, 0,
		ResourceFlags::RenderTarget, ResourceHeapType::Default,
		ResourceState::RenderTarget, ClearValue(1, 1, 1, 1), // // !IMPORTANT. Revealage is dst * prod(1-a)
		L"Transparency Revealage Buffer"
	));
	auto& accumalation_buffer_rtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DDesc(ResourceFormat::R16G16B16A16_FLOAT, 0),
		.viewed = accumalation_buffer
		});
	auto& accumalation_buffer_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R16G16B16A16_FLOAT, 0, 1),
		.viewed = accumalation_buffer
	});
	auto& revealage_buffer_rtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DDesc(ResourceFormat::R16_FLOAT, 0),
		.viewed = revealage_buffer
	});
	auto& revealage_buffer_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R16_FLOAT, 0, 1),
		.viewed = revealage_buffer
	});
	auto& frameBuffer = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R16G16B16A16_FLOAT, ResourceDimension::Texture2D,
		width, height, 1, 1, 1, 0,
		ResourceFlags::UnorderedAccess | ResourceFlags::RenderTarget, ResourceHeapType::Default,
		ResourceState::UnorderedAccess, ClearValue(0, 0, 0, 0),
		L"Frame Buffer"
	));
	auto& albedo_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R8G8B8A8_UNORM, 0, 1),
		.viewed = albedo
	});
	auto& normal_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R16G16_FLOAT, 0, 1),
		.viewed = normal
	});
	auto& material_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R8G8B8A8_UNORM, 0, 1),
		.viewed = material
	});
	auto& emissive_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R8G8B8A8_UNORM, 0, 1),
		.viewed = emissive
	});
	auto& velocity_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R16G16_FLOAT, 0, 1),
		.viewed = velocity
	});
	auto& fb_uav = rg.create<UnorderedAccessView>({
		.viewDesc = UnorderedAccessViewDesc::GetTexture2DDesc(ResourceFormat::R16G16B16A16_FLOAT,0,0),
		.viewed = frameBuffer
	});	
	auto& hiz_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R32_FLOAT, 0,Resource::ResourceDesc::numMipsOfDimension(width, height)),
		.viewed = hiz_buffer
	});
	std::vector<RgHandle> hiz_uavs;
	for (uint i = 0; i < Resource::ResourceDesc::numMipsOfDimension(width, height); i++)
		hiz_uavs.push_back(rg.create<UnorderedAccessView>({
			.viewDesc = UnorderedAccessViewDesc::GetTexture2DDesc(ResourceFormat::R32_FLOAT, i,0),
			.viewed = hiz_buffer
		}));	
	auto& fb_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R16G16B16A16_FLOAT, 0, 1),
		.viewed = frameBuffer
	});
	auto& fb_rtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DDesc(ResourceFormat::R16G16B16A16_FLOAT, 0),
		.viewed = frameBuffer
	});
	pass_Clear.insert_dsv(rg, sceneView, { &depth, &silhouetteDepth }, { &depth_dsv, &silhouetteDepth_dsv });
	pass_Clear.insert_rtv(rg, sceneView, { &frameBuffer, &albedo, &normal, &material, &emissive, &velocity }, { &fb_rtv, &albedo_rtv, &normal_rtv, &material_rtv, &emissive_rtv, &velocity_rtv });
	if (sceneView->get_SceneGlobals().numMeshInstances) {
		pass_IndirectCull.insert_earlycull(rg, sceneView, {
			.visibilityBuffer = instanceVisibility,
			.indirectCmdBuffer = indirectCmds,
			.indirectCmdBufferUAV = indirectCmdsUAV
			});
		pass_GBuffer.insert_earlydraw(rg, sceneView, {
			.indirectCommands = indirectCmds,
			.indirectCommandsUAV = indirectCmdsUAV,
			.depth = depth,
			.albedo = albedo,
			.normal = normal,
			.material = material,
			.emissive = emissive,
			.velocity = velocity,
			.depth_dsv = depth_dsv,
			.albedo_rtv = albedo_rtv,
			.normal_rtv = normal_rtv,
			.material_rtv = material_rtv,
			.emissive_rtv = emissive_rtv,
			.velocity_rtv = velocity_rtv
			});
		pass_HiZ.insert(rg, sceneView, {
			.depth = depth,
			.depthSRV = depth_srv,
			.hizTexture = hiz_buffer,
			.hizUAVs = hiz_uavs
			});
		pass_IndirectCull.insert_latecull(rg, sceneView, {
			.visibilityBuffer = instanceVisibility,
			.indirectCmdBuffer = indirectCmds,
			.indirectCmdBufferUAV = indirectCmdsUAV,
			.transparencyIndirectCmdBuffer = transparencyIndirectCmds,
			.transparencyIndirectCmdBufferUAV = transparencyIndirectCmdsUAV,
			.silhouetteIndirectCmdBuffer = silhouetteIndirectCmds,
			.silhouetteIndirectCmdBufferUAV = silhouetteIndirectCmdsUAV,
			.hizTexture = hiz_buffer,
			.hizSRV = hiz_srv
			});
		pass_GBuffer.insert_latedraw(rg, sceneView, {
			.indirectCommands = indirectCmds,
			.indirectCommandsUAV = indirectCmdsUAV,
			.depth = depth,
			.albedo = albedo,
			.normal = normal,
			.material = material,
			.emissive = emissive,
			.velocity = velocity,
			.depth_dsv = depth_dsv,
			.albedo_rtv = albedo_rtv,
			.normal_rtv = normal_rtv,
			.material_rtv = material_rtv,
			.emissive_rtv = emissive_rtv,
			.velocity_rtv = velocity_rtv
			});
		pass_Lighting.insert(rg, sceneView, {
			.frameBuffer = frameBuffer,
			.depth = depth,
			.albedo = albedo,
			.normal = normal,
			.material = material,
			.emissive = emissive,
			.depth_srv = depth_srv,
			.albedo_srv = albedo_srv,
			.normal_srv = normal_srv,
			.material_srv = material_srv,
			.emissive_srv = emissive_srv,
			.fb_uav = fb_uav
			});
		pass_Skybox.insert(rg, sceneView, {
			.depth = depth,
			.depthDSV = depth_dsv,
			.frameBuffer = frameBuffer,
			.frameBufferRTV = fb_rtv
		});
		pass_Transparency.insert(rg, sceneView, {
			.transparencyIndirectCommands = transparencyIndirectCmds,
			.transparencyIndirectCommandsUAV = transparencyIndirectCmdsUAV,
			.accumalationBuffer = accumalation_buffer,
			.revealageBuffer = revealage_buffer,
			.accumalationBuffer_rtv = accumalation_buffer_rtv,
			.accumalationBuffer_srv = accumalation_buffer_srv,
			.revealageBuffer_rtv = revealage_buffer_rtv,
			.revealageBuffer_srv = revealage_buffer_srv,
			.depth = depth,
			.depth_dsv = depth_dsv,
			.framebuffer = frameBuffer,
			.fb_uav = fb_uav,
			.material_rtv = material_rtv,
			.material = material
		});	
		pass_Tonemapping.insert(rg, sceneView, {
			.frameBuffer = frameBuffer,
			.frameBufferUAV = fb_uav
		});
		pass_Silhouette.insert(rg, sceneView, {
			.silhouetteCMD = silhouetteIndirectCmds,
			.silhouetteCMDUAV = silhouetteIndirectCmdsUAV,
			.silhouetteDepth = silhouetteDepth,
			.silhouetteDepthDSV = silhouetteDepth_dsv,
			.silhouetteDepthSRV = silhouetteDepth_srv,
			.frameBuffer = frameBuffer,
			.frameBufferUAV = fb_uav
		});
	}
	rg.get_epilogue_pass().read(frameBuffer).read(material);
	rg.execute(device->GetDefaultCommandList<CommandListType::Direct>());
	r_materialBufferSRV = rg.get<ShaderResourceView>(material_srv);
	r_materialBufferTex = rg.get<Texture>(material);
	r_frameBufferSRV = rg.get<ShaderResourceView>(fb_srv);
	r_frameBufferTex = rg.get<Texture>(frameBuffer);

}