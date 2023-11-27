#pragma once
#include "RenderPass.hpp"
#include "FFXSpd.hpp"

class IBLPrefilterPass {
	std::unique_ptr<RHI::Shader> Probe2CubemapCS;
	std::unique_ptr<RHI::RootSignature> RS;
	std::unique_ptr<RHI::PipelineState> Probe2CubemapPSO;

	FFXSPDPass spdPass;
public:
	struct IBLPrefilterPassHandles {
		RgHandle& panoSrv; // Orignial HDRI imported ShaderResourceView

		RgHandle& cubemapOut; // Original HDRI + Mips
		RgHandle& cubemapOutUAV; // MIP0 UAV
		std::vector<RgHandle> cubemapOutUAVs; // UAVs for all mips

		RgHandle& radianceOut; // Specular prefilter
		RgHandle& irradianceOut; // Diffuse prefilter
	};

	IBLPrefilterPass(RHI::Device* device);
	RenderGraphPass& insert(RenderGraph& rg, IBLPrefilterPassHandles&& handles);
};