#pragma once
#include "../Renderer.hpp"
struct ClearPass : public IRenderPass {
public:
	explicit ClearPass(RHI::Device* device) : IRenderPass(device, "Clear") {};
	void reset() override {};
	RenderGraphPass& insert_rtv(RenderGraph& rg, SceneView* sceneView, std::vector<std::pair<RgHandle*, RgHandle*>> const& texture_rtvs);
	RenderGraphPass& insert_dsv(RenderGraph& rg, SceneView* sceneView, std::vector<std::pair<RgHandle*, RgHandle*>> const& texture_dsvs);
};
