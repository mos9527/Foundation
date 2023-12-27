#pragma once
#include "../Renderer.hpp"
#define SPD_MAX_NUM_SLICES 0xffff
struct FFXSPDPass : public IRenderPass {	
	std::unique_ptr<RHI::Shader> CS;
	std::unique_ptr<RHI::PipelineState> PSO;

	std::unique_ptr<BufferContainer<FFXSpdConstant>> ffxPassConstants;
	std::unique_ptr<RHI::Buffer> ffxPassCounter;
	std::unique_ptr<RHI::UnorderedAccessView> ffxPassCounterUAV;

public:
	struct Handles {
		RgHandle& srcTexture;		
		RgHandle& dstTexture;
		std::array<RgHandle*, 32> dstMipUAVs;
	};
	std::wstring reduce_func = L"(v0 + v1 + v2 + v3) * 0.25";
	FFXSPDPass(RHI::Device* device) : IRenderPass(device, { L"FFXSpd" }, "FFX SPD") { reset(); };
	virtual void reset();
	RenderGraphPass& insert(RenderGraph& rg, Handles const& handles);
};