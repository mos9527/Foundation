#pragma once
#include "RenderPass.hpp"
#define SPD_MAX_NUM_SLICES 0xffff
class FFXSPDPass {	
	std::unique_ptr<RHI::Shader> ffxPassCS;
	std::unique_ptr<RHI::RootSignature> ffxPassRS;
	std::unique_ptr<RHI::PipelineState> ffxPassPSO;

	std::unique_ptr<RHI::Buffer> ffxPassConstants;
	std::unique_ptr<RHI::Buffer> ffxPassCounter;
	std::unique_ptr<RHI::UnorderedAccessView> ffxPassCounterUAV;
public:
	struct FFXSPDPassHandles {
		RgHandle& srcTexture;		
		RgHandle& dstTexture;
		std::vector<RgHandle> dstMipUAVs;
	};
	FFXSPDPass(RHI::Device* device);
	RenderGraphPass& insert(RenderGraph& rg, SceneGraphView* sceneView, FFXSPDPassHandles& handles);
};