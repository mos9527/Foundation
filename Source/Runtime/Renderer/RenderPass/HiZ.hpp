#pragma once
#include "RenderPass.hpp"
#include "FFXSpd.hpp"

class HierarchalDepthPass {	
	std::unique_ptr<RHI::Shader> depthSampleCS;
	std::unique_ptr<RHI::RootSignature> depthSampleRS;
	std::unique_ptr<RHI::PipelineState> depthSamplePSO;

	std::unique_ptr<RHI::Buffer> depthSampleConstants;
	FFXSPDPass spdPass;
public:
	struct HierarchalDepthPassHandles {
		RgHandle& depth; // imported. depth from previous frame
		RgHandle& depthSRV;

		RgHandle& hizTexture;
		std::vector<RgHandle> hizUAVs;
	};
	
	HierarchalDepthPass(RHI::Device* device);
	RenderGraphPass& insert(RenderGraph& rg, SceneGraphView* sceneView, HierarchalDepthPassHandles& handles);
};