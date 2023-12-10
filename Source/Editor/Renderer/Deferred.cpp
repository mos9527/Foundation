#include "Deferred.hpp"
using namespace RHI;
/*
* Deferred Rendering Pipeline
* :: GBuffer Layout ::
* ooookay this will need some more explaination later on
* TBD for now
*/
void DeferredRenderer::Render(SceneView* sceneView)
{
	ZoneScoped;
	UINT width = g_Editor.render.width, height = g_Editor.render.height;
	RenderGraph rg(cache);		
	/* BUFFERS */
	// Opaque / GBuffer
	auto& gbufferCmds = rg.create<Buffer>(InstanceCull::GetCountedIndirectCmdBufferDesc(L"Opaque GBuffer CMD"));
	auto& gbufferCmdsUAV = rg.create<UnorderedAccessView>(InstanceCull::GetCountedIndirectCmdBufferUAVDesc(gbufferCmds));
	// WBOIT
	auto& transparencyIndirectCmds = rg.create<Buffer>(InstanceCull::GetCountedIndirectCmdBufferDesc(L"Transparency CMD"));
	auto& transparencyIndirectCmdsUAV = rg.create<UnorderedAccessView>(InstanceCull::GetCountedIndirectCmdBufferUAVDesc(transparencyIndirectCmds));
	// Silhouette
	auto& silhouetteIndirectCmds = rg.create<Buffer>(InstanceCull::GetCountedIndirectCmdBufferDesc(L"Silhouette CMD"));
	auto& silhouetteIndirectCmdsUAV = rg.create<UnorderedAccessView>(InstanceCull::GetCountedIndirectCmdBufferUAVDesc(silhouetteIndirectCmds));
	
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
}