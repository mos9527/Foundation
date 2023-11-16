#include "Deferred.hpp"

void DeferredRenderer::Render() {
	RenderGraph rg(cache);
	sceneView->update();
}