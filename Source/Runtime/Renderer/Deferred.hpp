#pragma once
#include "Renderer.hpp"
#include "RenderPass/IndirectLODCull.hpp"
#include "RenderPass/GBuffer.hpp"
#include "RenderPass/DeferredLighting.hpp"
#include "RenderPass/HiZ.hpp"
class DeferredRenderer {
public:
	DeferredRenderer(RHI::Device* device) : device(device), pass_IndirectCull(device), pass_GBuffer(device), pass_HiZ(device), pass_Lighting(device) {};
	void SetViewportSize(uint width, uint height) {
		viewportSize.x = std::max(128u,width);
		viewportSize.y = std::max(128u,height);
	}
	uint2 GetViewportSize() { return viewportSize; }
	RHI::ShaderResourceView* Render(SceneGraphView* sceneView);
private:	
	RHI::Device* const device;

	RenderGraphResourceCache cache;

	uint2 viewportSize{ 128, 128 };

	IndirectLODCullPass pass_IndirectCull;
	GBufferPass pass_GBuffer;
	HierarchalDepthPass pass_HiZ;
	DeferredLightingPass pass_Lighting;
};