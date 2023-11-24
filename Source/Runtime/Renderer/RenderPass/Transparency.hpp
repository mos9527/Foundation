#pragma once
#include "RenderPass.hpp"

class TransparencyPass {
	std::unique_ptr<RHI::Shader> VS, PS, blendCS;
	std::unique_ptr<RHI::RootSignature> RS, blendRS;
	std::unique_ptr<RHI::PipelineState> PSO, PSO_Wireframe, PSO_Blend;
	std::unique_ptr<RHI::CommandSignature> IndirectCmdSig;
public:
	struct TransparencyPassHandles {
		RgHandle& transparencyIndirectCommands; // The one generated for transparency! don't mix them up
		RgHandle& transparencyIndirectCommandsUAV;

		RgHandle& accumalationBuffer;
		RgHandle& revealageBuffer;

		RgHandle& accumalationBuffer_rtv;
		RgHandle& accumalationBuffer_srv;

		RgHandle& revealageBuffer_rtv;
		RgHandle& revealageBuffer_srv;

		RgHandle& depth;
		RgHandle& depth_dsv;
		RgHandle& framebuffer;
		RgHandle& fb_uav;		
	};

	TransparencyPass(RHI::Device* device);
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, TransparencyPassHandles&& handles);
};