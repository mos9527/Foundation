#pragma once
#include "Renderer.hpp"
#include "RenderPass/GBuffer.hpp"
#include "RenderPass/DeferredLighting.hpp"
class DeferredRenderer {
public:
	DeferredRenderer(RHI::Device* device) : device(device), pass_GBuffer(device), pass_Lighting(device) {};
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

	GBufferPass pass_GBuffer;
	DeferredLightingPass pass_Lighting;
};