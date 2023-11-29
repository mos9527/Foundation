#pragma once
#include "../Renderer.hpp"

class TonemappingPass {
	std::unique_ptr<RHI::Shader> CS;
	std::unique_ptr<RHI::RootSignature> RS;
	std::unique_ptr<RHI::PipelineState> PSO;

public:
	struct TonemappingPassHandles {
		RgHandle& frameBuffer;
	};

	TonemappingPass(RHI::Device* device);
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, TonemappingPassHandles&& handles);
};