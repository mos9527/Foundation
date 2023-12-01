#pragma once
#include "../Renderer.hpp"

class TonemappingPass {
	std::unique_ptr<RHI::Shader> CS_Histogram, CS_Avg, CS_Tonemap;
	std::unique_ptr<RHI::RootSignature> RS;
	std::unique_ptr<RHI::PipelineState> PSO_Histogram, PSO_Avg, PSO_Tonemap;

	std::unique_ptr<RHI::Buffer> histogramBuffer;
	std::unique_ptr<RHI::UnorderedAccessView> histogramBufferUAV;

	std::unique_ptr<RHI::Texture> luminanceBuffer;
	std::unique_ptr<RHI::UnorderedAccessView> luminanceBufferUAV;

	RHI::Device* const device;
public:
	struct TonemappingPassHandles {
		RgHandle& frameBuffer;
		RgHandle& frameBufferUAV;
	};

	TonemappingPass(RHI::Device* device);
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, TonemappingPassHandles&& handles);
};