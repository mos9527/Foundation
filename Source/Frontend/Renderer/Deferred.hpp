#pragma once
#include "Renderer.hpp"
class DeferredRenderer : public Renderer {
public:
	DeferredRenderer(AssetRegistry& assets, SceneGraph& scene, RHI::Device* device, RHI::Swapchain* swapChain) : Renderer(assets, scene, device, swapChain) {
		sceneView = std::make_unique<SceneGraphView>(device, scene);
	};
private:	
	std::unique_ptr<SceneGraphView> sceneView;
};