#pragma once
#include "../Processor.hpp"
#include "../../Renderer/RenderPass/FFXSpd.hpp"

class IBLPrefilterPass {
	std::unique_ptr<RHI::Shader> Probe2CubemapCS, PrefilterCS, LUTCS;	
	std::unique_ptr<RHI::PipelineState> Probe2CubemapPSO, PrefilterPSO, LUTPSO;
	FFXSPDPass spdPass;

	std::unique_ptr<BufferContainer<IBLPrefilterConstant>> constants;
public:
	struct IBLPrefilterPassHandles {
		RgHandle& panoSrv; // Orignial HDRI imported ShaderResourceView

		RgHandle& cubemap;
		std::array<RgHandle*, 32> cubemapUAVs;
		RgHandle& cubemapSRV;

		RgHandle& radianceCubeArray;
		std::array<RgHandle*, 32> radianceCubeArrayUAVs;
		RgHandle& radianceCubeArraySRV;

		RgHandle& irradianceCube;
		RgHandle& irradianceCubeUAV;
		RgHandle& irradianceCubeSRV;

		RgHandle& lutArray;
		RgHandle& lutArrayUAV;
		RgHandle& lutArraySRV;
	};

	IBLPrefilterPass(RHI::Device* device);
	RenderGraphPass& insert_pano2cube(RenderGraph& rg, IBLPrefilterPassHandles const& handles);
	RenderGraphPass& insert_diffuse_prefilter(RenderGraph& rg, IBLPrefilterPassHandles const& handles);
	RenderGraphPass& insert_specular_prefilter(RenderGraph& rg, IBLPrefilterPassHandles const& handles, uint mipIndex, uint mipLevels, uint cubeIndex);
	RenderGraphPass& insert_lut(RenderGraph& rg, IBLPrefilterPassHandles const& handles);
};