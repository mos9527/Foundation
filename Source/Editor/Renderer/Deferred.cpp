#include "Deferred.hpp"
using namespace RHI;
/*
* Deferred Rendering Pipeline
* :: GBuffer Layout ::
* ooookay this will need some more explaination later on
* TBD for now
*/
void DeferredRenderer::Render(SceneView* sceneView, CommandList* ctx)
{
	// xxx small object allocation optimization
	// Profiling shows the handle creatation process is taking ~1ms (!)
	// RgHandles have known sizes thus implementing a pool allocator shouldn't be too much a hassle		
	ZoneScoped;
	TracyMessageL("RG Ctor");
	RenderGraph rg(cache);		
	UINT width = g_Editor.render.width, height = g_Editor.render.height;

#pragma region Resource Handle Setup
	TracyMessageL("Build Handles");
	/* BUFFERS */
	// Opaque / GBuffer
	auto& gbufferCmds = rg.create<Buffer>(InstanceCull::GetCountedIndirectCmdBufferDesc(L"Opaque GBuffer CMD"));
	auto& gbufferCmdsUav = rg.create<UnorderedAccessView>(InstanceCull::GetCountedIndirectCmdBufferUAVDesc(gbufferCmds));
	// WBOIT
	auto& oitCmds = rg.create<Buffer>(InstanceCull::GetCountedIndirectCmdBufferDesc(L"Transparency CMD"));
	auto& oitCmdsUav = rg.create<UnorderedAccessView>(InstanceCull::GetCountedIndirectCmdBufferUAVDesc(oitCmds));
	// Silhouette
	auto& silhouetteCmds = rg.create<Buffer>(InstanceCull::GetCountedIndirectCmdBufferDesc(L"Silhouette CMD"));
	auto& silhouetteCmdsUav = rg.create<UnorderedAccessView>(InstanceCull::GetCountedIndirectCmdBufferUAVDesc(silhouetteCmds));

	/* TEXTURES */
	// GBuffers
	auto& gbufferTangentFrame = rg.create<Texture>(GBufferPass::GetTangentFrameDesc(width, height));
	auto& gbufferGradient = rg.create<Texture>(GBufferPass::GetGradientDesc(width, height));
	auto& gbufferMaterial = rg.create<Texture>(GBufferPass::GetMaterialDesc(width, height));
	auto& gbufferDepth = rg.create<Texture>(GBufferPass::GetDepthDesc(width, height));
	auto& gbufferDepthPyramid = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R32_FLOAT, ResourceDimension::Texture2D,
		width, height, Resource::ResourceDesc::numMipsOfDimension(width, height), 1, 1, 0,
		ResourceFlags::UnorderedAccess, ResourceHeapType::Default,
		ResourceState::UnorderedAccess, {}, L"Hierarchal Z Buffer"
	));
	// WBOIT
	auto& oitAccumalation = rg.create<Texture>(TransparencyPass::GetAccumalationDesc(width, height));
	auto& oitRevelage = rg.create<Texture>(TransparencyPass::GetRevealgeDesc(width, height));
	// Framebuffer
	auto& frameBuffer = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R16G16B16A16_FLOAT, ResourceDimension::Texture2D,
		width, height, 1, 1, 1, 0,
		ResourceFlags::UnorderedAccess | ResourceFlags::RenderTarget, ResourceHeapType::Default,
		ResourceState::UnorderedAccess, ClearValue(0, 0, 0, 0),
		L"Frame Buffer"
	));
	// Silhouette
	auto& silhouetteDepth = rg.create<Texture>(GBufferPass::GetDepthDesc(width, height, L"Silhouette Depth"));

	/* RESOURCE VIEWS*/
	// GBuffers
	auto& gbufferTangentFrameRtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DDesc(ResourceFormat::R10G10B10A2_UNORM, 0),
		.viewed = gbufferTangentFrame
	});
	auto& gbufferGradientRtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DDesc(ResourceFormat::R16G16B16A16_SNORM, 0),
		.viewed = gbufferGradient
	});
	auto& gbufferMaterialRtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DDesc(ResourceFormat::R16G16B16A16_UNORM, 0),
		.viewed = gbufferMaterial
	});

	auto& gbufferTangentFrameSrv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R10G10B10A2_UNORM,0,1),
		.viewed = gbufferTangentFrame
	});
	auto& gbufferGradientSrv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R16G16B16A16_SNORM,0,1),
		.viewed = gbufferGradient
	});
	auto& gbufferMaterialSrv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R16G16B16A16_UNORM,0,1),
		.viewed = gbufferMaterial
	});

	auto& gbufferDepthDsv = rg.create<DepthStencilView>({
		.viewDesc = DepthStencilViewDesc::GetTexture2DDepthBufferDesc(ResourceFormat::D32_FLOAT,0),
		.viewed = gbufferDepth
	});
	auto& gbufferDepthSrv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R32_FLOAT,0,1),
		.viewed = gbufferDepth
	});

	auto& gbufferDepthPyramidSrv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R32_FLOAT, 0,Resource::ResourceDesc::numMipsOfDimension(width, height)),
		.viewed = gbufferDepthPyramid
	});
	std::array<RgHandle*, 32> gbufferDepthPyramidUavs;
	for (uint i = 0; i < Resource::ResourceDesc::numMipsOfDimension(width, height); i++)
		gbufferDepthPyramidUavs[i] = &rg.create<UnorderedAccessView>({
			.viewDesc = UnorderedAccessViewDesc::GetTexture2DDesc(ResourceFormat::R32_FLOAT, i,0),
			.viewed = gbufferDepthPyramid
	});

	// WBOIT
	auto& oitAccumalationRtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DDesc(ResourceFormat::R16G16B16A16_FLOAT, 0),
		.viewed = oitAccumalation
	});
	auto& oitAccumalationSrv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R16G16B16A16_FLOAT, 0, 1),
		.viewed = oitAccumalation
	});
	auto& oitRevelageRtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DDesc(ResourceFormat::R16_FLOAT, 0),
		.viewed = oitRevelage
	});
	auto& oitRevelageSrv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R16_FLOAT, 0, 1),
		.viewed = oitRevelage
	});

	// Framebuffer
	auto& frameBufferSrv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R16G16B16A16_FLOAT, 0, 1),
		.viewed = frameBuffer
	});
	auto& frameBufferRtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DDesc(ResourceFormat::R16G16B16A16_FLOAT, 0),
		.viewed = frameBuffer
	});
	auto& frameBufferUav = rg.create<UnorderedAccessView>({
		.viewDesc = UnorderedAccessViewDesc::GetTexture2DDesc(ResourceFormat::R16G16B16A16_FLOAT,0,0),
		.viewed = frameBuffer
	});

	// Silhouette
	auto& silhouetteDsv = rg.create<DepthStencilView>({
		.viewDesc = DepthStencilViewDesc::GetTexture2DDepthBufferDesc(ResourceFormat::D32_FLOAT, 0),
		.viewed = silhouetteDepth
	});
	auto& silhouetteSrv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R32_FLOAT, 0, 1),
		.viewed = silhouetteDepth
	});
#pragma endregion
	TracyMessageL("Insert");
	pass_Clear.insert_dsv(rg, sceneView, { { &gbufferDepth,&gbufferDepthDsv } , { &silhouetteDepth,&silhouetteDsv } });
	pass_Clear.insert_rtv(rg, sceneView, { 
		{ &frameBuffer, &frameBufferRtv }, 
		{ &gbufferMaterial, &gbufferMaterialRtv }, 
		{ &gbufferGradient, &gbufferGradientRtv }, 
		{ &gbufferTangentFrame, &gbufferTangentFrameRtv },
		{ &oitAccumalation,&oitAccumalationRtv } ,
		{ &oitRevelage, &oitRevelageRtv }
	});
	if (sceneView->GetGlobalsBuffer().Data()->meshInstances.count) {
		pass_IndirectCull.insert(rg, sceneView, {
			.cmd_uav_instanceMaskAllow_instanceMaskRejcectClearCounter = {{
				{ &gbufferCmds,&gbufferCmdsUav, INSTANCE_FLAG_ENABLED | INSTANCE_FLAG_OPAQUE, 0, true },
				{ &oitCmds,&oitCmdsUav, INSTANCE_FLAG_ENABLED | INSTANCE_FLAG_TRANSPARENT, 0, true },
				{ &silhouetteCmds,&silhouetteCmdsUav, INSTANCE_FLAG_ENABLED | INSTANCE_FLAG_SILHOUETTE, 0, true }
			}}
			}, false);
		pass_GBuffer.insert(rg, sceneView, {
			.command_uav = { &gbufferCmds,&gbufferCmdsUav },
			.tangentframe_rtv = { &gbufferTangentFrame, &gbufferTangentFrameRtv },
			.gradient_rtv = { &gbufferGradient, &gbufferGradientRtv },
			.material_rtv = { &gbufferMaterial, &gbufferMaterialRtv  },
			.depth_dsv = { &gbufferDepth, &gbufferDepthDsv }
			});
		pass_HiZ.insert(rg, sceneView, {
			.depth_srv = { &gbufferDepth, &gbufferDepthSrv },
			.depthPyramid_MipUavs = { &gbufferDepthPyramid, gbufferDepthPyramidUavs  }
		});
		pass_IndirectCull.insert(rg, sceneView, {
			.hiz_srv = { &gbufferDepthPyramid, &gbufferDepthPyramidSrv },
			.cmd_uav_instanceMaskAllow_instanceMaskRejcectClearCounter = {{
				{ &gbufferCmds,&gbufferCmdsUav, INSTANCE_FLAG_ENABLED | INSTANCE_FLAG_OPAQUE, 0, true },
				{ &oitCmds,&oitCmdsUav, INSTANCE_FLAG_ENABLED | INSTANCE_FLAG_TRANSPARENT, 0, false },
				{ &silhouetteCmds,&silhouetteCmdsUav, INSTANCE_FLAG_ENABLED | INSTANCE_FLAG_SILHOUETTE, 0, false }
			}}
		}, true);
		pass_GBuffer.insert(rg, sceneView, {
			.command_uav = { &gbufferCmds,&gbufferCmdsUav },
			.tangentframe_rtv = { &gbufferTangentFrame, &gbufferTangentFrameRtv },
			.gradient_rtv = { &gbufferGradient, &gbufferGradientRtv },
			.material_rtv = { &gbufferMaterial, &gbufferMaterialRtv  },
			.depth_dsv = { &gbufferDepth, &gbufferDepthDsv }
		});
		pass_Lighting.insert(rg, sceneView, {
			.tangentframe_srv = { &gbufferTangentFrame, &gbufferTangentFrameSrv },
			.gradient_srv = { &gbufferGradient, &gbufferGradientSrv },
			.material_srv = { &gbufferMaterial, &gbufferMaterialSrv },
			.depth_srv = { &gbufferDepth, &gbufferDepthSrv },
			.framebuffer_uav = { &frameBuffer, &frameBufferUav }
		});
		pass_Skybox.insert(rg, sceneView, {
			.depth_dsv = { &gbufferDepth, &gbufferDepthDsv },
			.framebuffer_rtv = { &frameBuffer, &frameBufferRtv }
		});
		pass_Transparency.insert(rg, sceneView, {
			.command_uav = { &oitCmds,&oitCmdsUav },
			.accumaltion_rtv_srv = { &oitAccumalation, &oitAccumalationRtv, &oitAccumalationSrv },
			.revealage_rtv_srv = { &oitRevelage, &oitRevelageRtv, &oitRevelageSrv },
			.depth_dsv = { &gbufferDepth, &gbufferDepthDsv },
			.framebuffer_uav = { &frameBuffer, &frameBufferUav },
			.material_rtv_srv = { &gbufferMaterial, &gbufferMaterialRtv, &gbufferMaterialSrv }
		});
		pass_Tonemapping.insert(rg, sceneView, {
			.framebuffer_uav = { &frameBuffer, &frameBufferUav }
		});
		pass_Silhouette.insert(rg, sceneView, {
			.command_uav = { &silhouetteCmds, &silhouetteCmdsUav },
			.silhouette_dsv_srv = { &silhouetteDepth, &silhouetteDsv, &silhouetteSrv },
			.framebuffer_uav = { &frameBuffer, &frameBufferUav }
		});
	}
	TracyMessageL("Execute");
	rg.get_epilogue_pass().read(frameBuffer).read(gbufferMaterial);
	rg.execute(ctx);

	r_materialBufferSRV = rg.get<ShaderResourceView>(gbufferMaterialSrv);
	r_materialBufferTex = rg.get<Texture>(gbufferMaterial);
	r_frameBufferSRV = rg.get<ShaderResourceView>(frameBufferSrv);
	r_frameBufferTex = rg.get<Texture>(frameBuffer);
}