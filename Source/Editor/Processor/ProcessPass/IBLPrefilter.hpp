#pragma once
#include "../Processor.hpp"
#include "../../Renderer/RenderPass/FFXSpd.hpp"

class IBLPrefilterPass {
	std::unique_ptr<RHI::Shader> Probe2CubemapCS, PrefilterCS;	
	std::unique_ptr<RHI::PipelineState> Probe2CubemapPSO, PrefilterPSO;	
	FFXSPDPass spdPass;

	std::unique_ptr<BufferContainer<IBLPrefilterConstant>> constants;
public:
	struct IBLPrefilterPassHandles {
		RgHandle& panoSrv; // Orignial HDRI imported ShaderResourceView

		RgHandle& cubemap;
		std::array<RgHandle*, 16> cubemapUAVs;
		RgHandle& cubemapSRV;

		RgHandle& radianceCubeArray;
		std::array<RgHandle*,16> radianceCubeArrayUAVs;
		RgHandle& radianceCubeArraySRV;

		RgHandle& irradianceCube;
		RgHandle& irradianceCubeUAV;
		RgHandle& irradianceCubeSRV;
	};

	IBLPrefilterPass(RHI::Device* device);
	RenderGraphPass& insert_pano2cube(RenderGraph& rg, IBLPrefilterPassHandles const& handles);
	RenderGraphPass& insert_diffuse_prefilter(RenderGraph& rg, IBLPrefilterPassHandles const& handles);
	RenderGraphPass& insert_specular_prefilter(RenderGraph& rg, IBLPrefilterPassHandles const& handles, uint mipIndex, uint mipLevels, uint cubeIndex);
	RenderGraphPass& insert_lut(RenderGraph& rg, IBLPrefilterPassHandles const& handles);
};