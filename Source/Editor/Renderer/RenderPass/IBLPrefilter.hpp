#pragma once
#include "RenderPass.hpp"
#include "FFXSpd.hpp"

class IBLPrefilterPass {
	std::unique_ptr<RHI::Shader> HDRI2CubemapCS;
	std::unique_ptr<RHI::RootSignature> HDRI2CubemapRS;
	std::unique_ptr<RHI::PipelineState> HDRI2CubemapPSO;

	FFXSPDPass spdPass;
public:
	struct IBLPrefilterPassHandles {
		RHI::ShaderResourceView* HDRI;
		RgHandle& cubemapOut; // Original HDRI + Mips
		RgHandle& cubemapOutUAV;

		RgHandle& radianceOut; // Specular prefilter
		RgHandle& irridianceOut; // Diffuse prefilter
	};

	IBLPrefilterPass(RHI::Device* device);
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, IBLPrefilterPassHandles&& handles);
};