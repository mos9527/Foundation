#pragma once
#include "../Renderer.hpp"

struct SkyboxPass : public IRenderPass {
	std::unique_ptr<RHI::Shader> VS,PS;
	std::unique_ptr<RHI::PipelineState> PSO;
	std::unique_ptr<RHI::Buffer> IB,VB;
	std::unique_ptr<BufferContainer<SkyboxConstants>> constants;
	void UploadCubeMesh();
public:
	struct Handles {
		RgHandle& depth; // pre-transparency
		RgHandle& depthDSV;

		RgHandle& frameBuffer;
		RgHandle& frameBufferRTV;		
	};

	SkyboxPass(RHI::Device* device) : IRenderPass(device, "Skybox") { reset(); };
	virtual void reset();
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles);
};