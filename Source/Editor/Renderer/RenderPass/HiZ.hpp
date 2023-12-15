#pragma once
#include "../Renderer.hpp"
#include "FFXSpd.hpp"
struct HierarchalDepthPass : public IRenderPass {	
	std::unique_ptr<RHI::Shader> CS;
	std::unique_ptr<RHI::PipelineState> PSO;

	std::unique_ptr<BufferContainer<DepthSampleToTextureConstant>> constants;
	FFXSPDPass spdPass;
public:
	struct Handles {
		std::pair<RgHandle*, RgHandle*> depth_srv;
		std::pair<RgHandle*, std::array<RgHandle*, 16>> depthPyramid_MipUavs;
	};
	
	HierarchalDepthPass(RHI::Device* device) : IRenderPass(device, {L"DepthSampleToTexture"}, "HiZ"), spdPass(device) { reset(); };
	virtual void reset();

	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles);
};