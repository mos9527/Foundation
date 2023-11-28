#pragma once
#include "../Renderer.hpp"
#include "FFXSpd.hpp"
class HierarchalDepthPass {	
	std::unique_ptr<RHI::Shader> CS;
	std::unique_ptr<RHI::RootSignature> RS;
	std::unique_ptr<RHI::PipelineState> PSO;

	std::unique_ptr<RHI::Buffer> depthSampleConstants;
	FFXSPDPass spdPass;
public:
	struct HierarchalDepthPassHandles {
		RgHandle& depth; 
		RgHandle& depthSRV;

		RgHandle& hizTexture;
		std::vector<RgHandle> hizUAVs;
	};
	
	HierarchalDepthPass(RHI::Device* device);
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, HierarchalDepthPassHandles&& handles);
};