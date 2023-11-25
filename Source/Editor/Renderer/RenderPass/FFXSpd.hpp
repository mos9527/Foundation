#pragma once
#include "RenderPass.hpp"
#define SPD_MAX_NUM_SLICES 0xffff
class FFXSPDPass {	
	std::unique_ptr<RHI::Shader> CS;
	std::unique_ptr<RHI::RootSignature> RS;
	std::unique_ptr<RHI::PipelineState> PSO;

	std::unique_ptr<RHI::Buffer> ffxPassConstants;
	std::unique_ptr<RHI::Buffer> ffxPassCounter;
	std::unique_ptr<RHI::UnorderedAccessView> ffxPassCounterUAV;
public:
	struct FFXSPDPassHandles {
		RgHandle& srcTexture;		
		RgHandle& dstTexture;
		std::vector<RgHandle> dstMipUAVs;
	};
	FFXSPDPass(RHI::Device* device, const wchar_t* reduce=L"(v0 + v1 + v2 + v3) * 0.25");
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, FFXSPDPassHandles&& handles);
};