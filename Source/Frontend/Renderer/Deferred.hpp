#pragma once
#include "Renderer.hpp"
class DeferredRenderer : public Renderer {
public:
	DeferredRenderer(AssetRegistry& assets, SceneGraph& scene, RHI::Device* device) : Renderer(assets, scene, device) {
		sceneView = std::make_unique<SceneGraphView>(device, scene);
	};
	virtual void Render();
private:	
	std::unique_ptr<SceneGraphView> sceneView;
};