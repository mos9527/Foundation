#include "Deferred.hpp"
using namespace RHI;
RHI::ShaderResourceView* DeferredRenderer::Render(SceneView* sceneView)
{
	UINT width = sceneView->get_SceneGlobals().frameDimension.x, height = sceneView->get_SceneGlobals().frameDimension.y;
	RenderGraph rg(cache);	
	/* Resources */
	auto& indirectCmds = rg.create<Buffer>(pass_IndirectCull.GetCountedIndirectCmdBufferDesc());
	auto& indirectCmdsUAV = rg.create<UnorderedAccessView>(pass_IndirectCull.GetCountedIndirectCmdBufferUAVDesc(indirectCmds));
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
	auto& albedo_rtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DRenderTargetDesc(ResourceFormat::R8G8B8A8_UNORM, 0),
		.viewed = albedo
	});
	auto& normal_rtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DRenderTargetDesc(ResourceFormat::R16G16_FLOAT, 0),
		.viewed = normal
	});
	auto& material_rtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DRenderTargetDesc(ResourceFormat::R8G8B8A8_UNORM, 0),
		.viewed = material
	});
	auto& emissive_rtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DRenderTargetDesc(ResourceFormat::R8G8B8A8_UNORM, 0),
		.viewed = emissive
	});
	auto& hiz_buffer = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R32_FLOAT, ResourceDimension::Texture2D,
		width, height, Resource::ResourceDesc::numMipsOfDimension(width, height), 1, 1, 0,
		ResourceFlags::UnorderedAccess, ResourceHeapType::Default,
		ResourceState::UnorderedAccess, {}, L"Hierarchal Z Buffer"
	));
	auto& frameBuffer = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R8G8B8A8_UNORM, ResourceDimension::Texture2D,
		width, height, 1, 1, 1, 0,
		ResourceFlags::UnorderedAccess, ResourceHeapType::Default,
		ResourceState::UnorderedAccess, {},
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
	auto& fb_uav = rg.create<UnorderedAccessView>({
		.viewDesc = UnorderedAccessViewDesc::GetTexture2DDesc(ResourceFormat::R8G8B8A8_UNORM,0,0),
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
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R8G8B8A8_UNORM, 0, 1),
		.viewed = frameBuffer
	});
	auto& p1 = pass_IndirectCull.insert_earlycull(rg, sceneView, {
		.visibilityBuffer = instanceVisibility,
		.indirectCmdBuffer = indirectCmds,
		.indirectCmdBufferUAV = indirectCmdsUAV
	});
	auto& p2 = pass_GBuffer.insert_earlydraw(rg, sceneView, {
		.indirectCommands = indirectCmds,
		.indirectCommandsUAV = indirectCmdsUAV,
		.depth = depth,
		.albedo = albedo,
		.normal = normal,
		.material = material,
		.emissive = emissive,
		.depth_dsv = depth_dsv,
		.albedo_rtv = albedo_rtv,
		.normal_rtv = normal_rtv,
		.material_rtv = material_rtv,
		.emissive_rtv = emissive_rtv
	});
	auto& p3 = pass_HiZ.insert(rg, sceneView, {
		.depth = depth,
		.depthSRV = depth_srv,
		.hizTexture = hiz_buffer,
		.hizUAVs = hiz_uavs
	});
	auto& p4 = pass_IndirectCull.insert_latecull(rg, sceneView, {
		.visibilityBuffer = instanceVisibility,
		.indirectCmdBuffer = indirectCmds,
		.indirectCmdBufferUAV = indirectCmdsUAV,
		.hizTexture = hiz_buffer,
		.hizSRV = hiz_srv
	});
	auto& p5 = pass_GBuffer.insert_latedraw(rg, sceneView, {
		.indirectCommands = indirectCmds,
		.indirectCommandsUAV = indirectCmdsUAV,
		.depth = depth,
		.albedo = albedo,
		.normal = normal,
		.material = material,
		.emissive = emissive,
		.depth_dsv = depth_dsv,
		.albedo_rtv = albedo_rtv,
		.normal_rtv = normal_rtv,
		.material_rtv = material_rtv,
		.emissive_rtv = emissive_rtv
	});
	auto& p6 = pass_Lighting.insert(rg, sceneView, {
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
	rg.get_epilogue_pass().read(frameBuffer);
	rg.execute(device->GetDefaultCommandList<CommandListType::Direct>());
	return rg.get<ShaderResourceView>(fb_srv);
}