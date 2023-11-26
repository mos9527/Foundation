#pragma once
#include "Renderer.hpp"
#include "RenderPass/Clear.hpp"
#include "RenderPass/IndirectLODCull.hpp"
#include "RenderPass/GBuffer.hpp"
#include "RenderPass/DeferredLighting.hpp"
#include "RenderPass/HiZ.hpp"
#include "RenderPass/Transparency.hpp"
class DeferredRenderer {
public:
	DeferredRenderer(RHI::Device* device) : 
		device(device), pass_Clear(device), pass_IndirectCull(device), pass_GBuffer(device), pass_HiZ(device), pass_Lighting(device),
		pass_Transparency(device)
	{};	
	RHI::ShaderResourceView* Render(SceneView* sceneView);
private:	
	RHI::Device* const device;

	RenderGraphResourceCache cache;

	uint2 viewportSize{ 128, 128 };
	
	ClearPass pass_Clear;
	IndirectLODCullPass pass_IndirectCull;
	GBufferPass pass_GBuffer;
	HierarchalDepthPass pass_HiZ;
	DeferredLightingPass pass_Lighting;
	TransparencyPass pass_Transparency;
};