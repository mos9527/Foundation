#include "Tonemapping.hpp"
using namespace RHI;
TonemappingPass::TonemappingPass(Device* device) {

}

RenderGraphPass& TonemappingPass::insert(RenderGraph& rg, SceneView* sceneView, TonemappingPassHandles&& handles) {
	return rg.add_pass();
}