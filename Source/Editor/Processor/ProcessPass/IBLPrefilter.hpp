#pragma once
#include "../Processor.hpp"
#include "../../Renderer/RenderPass/FFXSpd.hpp"

class IBLPrefilterPass {
	std::unique_ptr<RHI::Shader> Probe2CubemapCS, PrefilterCS, LUTCS;
	std::unique_ptr<RHI::RootSignature> RS;
	std::unique_ptr<RHI::PipelineState> Probe2CubemapPSO, PrefilterPSO, LUTPSO;	
	FFXSPDPass spdPass;
public:
	struct IBLPrefilterPassHandles {
		RgHandle& panoSrv; // Orignial HDRI imported ShaderResourceView

		RgHandle& cubemap;
		std::vector<RgHandle> cubemapUAVs;
		RgHandle& cubemapSRV;

		RgHandle& radianceCubeArray;
		std::vector<RgHandle> radianceCubeArrayUAVs;
		RgHandle& radianceCubeArraySRV;

		RgHandle& irradianceCube;
		RgHandle& irradianceCubeUAV;
		RgHandle& irradianceCubeSRV;

		RgHandle& lutArray;
		RgHandle& lutArrayUAV;
		RgHandle& lutArraySRV;
	};

	IBLPrefilterPass(RHI::Device* device);
	RenderGraphPass& insert_pano2cube(RenderGraph& rg, IBLPrefilterPassHandles& handles);
	RenderGraphPass& insert_diffuse_prefilter(RenderGraph& rg, IBLPrefilterPassHandles& handles);
	RenderGraphPass& insert_specular_prefilter(RenderGraph& rg, IBLPrefilterPassHandles& handles, uint mipIndex, uint mipLevels, uint cubeIndex);
	RenderGraphPass& insert_lut(RenderGraph& rg, IBLPrefilterPassHandles& handles);
};