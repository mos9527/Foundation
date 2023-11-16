#include "Deferred.hpp"

void DeferredRenderer::Render() {
	RenderGraph rg(cache);
	scene.get_active_camera().aspect = swapchain->GetAspect();
	sceneView->update();
}