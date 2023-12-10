#pragma once
#include "../Renderer.hpp"

struct TonemappingPass : public IRenderPass {
	std::unique_ptr<RHI::Shader> CS_Histogram, CS_Avg, CS_Tonemap;
	std::unique_ptr<RHI::PipelineState> PSO_Histogram, PSO_Avg, PSO_Tonemap;

	std::unique_ptr<RHI::Buffer> histogramBuffer;
	std::unique_ptr<RHI::UnorderedAccessView> histogramBufferUAV;

	std::unique_ptr<RHI::Texture> luminanceBuffer;
	std::unique_ptr<RHI::UnorderedAccessView> luminanceBufferUAV;

	std::unique_ptr<BufferContainer<TonemappingConstants>> constants;
public:
	struct Handles {
		std::pair<RgHandle*, RgHandle*> framebuffer_uav;
	};

	TonemappingPass(RHI::Device* device) : IRenderPass(device, "Tonemapping") { reset(); };
	virtual void reset();
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles);
};