#pragma once
#include "Renderer.hpp"
class DeferredRenderer : public Renderer {
public:
	DeferredRenderer(AssetRegistry& assets, SceneGraph& scene, RHI::Device* device, RHI::Swapchain* swapchain) : Renderer(assets, scene, device, swapchain) {
		sceneView = std::make_unique<SceneGraphView>(device, scene);
	};
	virtual void Render();
private:	
	std::unique_ptr<SceneGraphView> sceneView;
	RenderGraphResourceCache cache;
};