#pragma once
#include "Renderer.hpp"
#include "RenderPass/Clear.hpp"
#include "RenderPass/IndirectLODCull.hpp"
#include "RenderPass/GBuffer.hpp"
#include "RenderPass/DeferredLighting.hpp"
#include "RenderPass/HiZ.hpp"
#include "RenderPass/Transparency.hpp"
#include "RenderPass/SilhouettePass.hpp"
class DeferredRenderer {
public:
	RHI::ShaderResourceView* r_frameBufferSRV, * r_materialBufferSRV;
	RHI::Texture* r_frameBufferTex, * r_materialBufferTex;
	DeferredRenderer(RHI::Device* device) : 
		device(device), pass_Clear(device), pass_IndirectCull(device), pass_GBuffer(device), pass_HiZ(device), pass_Lighting(device),
		pass_Transparency(device), pass_Silhouette(device)
	{};	
	void Render(SceneView* sceneView);
private:	
	RHI::Device* const device;

	RenderGraphResourceCache cache;
	
	ClearPass pass_Clear;
	IndirectLODCullPass pass_IndirectCull;
	GBufferPass pass_GBuffer;
	HierarchalDepthPass pass_HiZ;
	DeferredLightingPass pass_Lighting;
	TransparencyPass pass_Transparency;
	SilhouettePass pass_Silhouette;
};