#pragma once
#include "../Renderer.hpp"

class SkyboxPass {
	std::unique_ptr<RHI::Shader> VS,PS;
	std::unique_ptr<RHI::RootSignature> RS;
	std::unique_ptr<RHI::PipelineState> PSO;
	std::unique_ptr<RHI::Buffer> IB,VB;

	void UploadCubeMesh();
	RHI::Device* const device;
public:
	struct SkyboxPassHandles {
		RgHandle& depth; // pre-transparency
		RgHandle& depthDSV;

		RgHandle& frameBuffer;
		RgHandle& frameBufferRTV;		
	};

	SkyboxPass(RHI::Device* device);
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, SkyboxPassHandles&& handles);
};