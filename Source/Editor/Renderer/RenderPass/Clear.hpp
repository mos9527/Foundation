#pragma once
#include "../Renderer.hpp"
struct ClearPass : public IRenderPass {
public:
	ClearPass(RHI::Device* device) : IRenderPass(device, "Clear") {};
	virtual void reset() {};
	RenderGraphPass& insert_rtv(RenderGraph& rg, SceneView* sceneView, std::vector<std::pair<RgHandle*, RgHandle*>> const& texture_rtvs);
	RenderGraphPass& insert_dsv(RenderGraph& rg, SceneView* sceneView, std::vector<std::pair<RgHandle*, RgHandle*>> const& texture_dsvs);
};
