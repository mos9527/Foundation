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
		RgHandle& depth; 
		RgHandle& depthSRV;

		RgHandle& hizTexture;
		std::vector<RgHandle> hizUAVs;
	};
	
	HierarchalDepthPass(RHI::Device* device) : IRenderPass(device), spdPass(device) { reset(); };
	virtual void reset();

	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles);
};