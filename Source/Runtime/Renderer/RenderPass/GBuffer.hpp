#pragma once
#include "RenderPass.hpp"

class GBufferPass {
public:
	struct GBufferPassHandles {
		RgHandle& indirectCommands;
		RgHandle& indirectCommandsUAV;

		RgHandle& depth;

		RgHandle& albedo;
		RgHandle& normal;
		RgHandle& material;
		RgHandle& emissive;
		RgHandle& velocity;

		RgHandle& depth_dsv;

		RgHandle& albedo_rtv;
		RgHandle& normal_rtv;
		RgHandle& material_rtv;
		RgHandle& emissive_rtv;
		RgHandle& velocity_rtv;

	};
private:
	std::unique_ptr<RHI::Shader> VS, PS;
	std::unique_ptr<RHI::RootSignature> RS;
	std::unique_ptr<RHI::PipelineState> PSO, PSO_Wireframe;
	std::unique_ptr<RHI::CommandSignature> IndirectCmdSig;

	void insert_execute(RenderGraphPass& pass, SceneView* sceneView, GBufferPassHandles&& handles, bool late);
public:

	GBufferPass(RHI::Device* device);
	RenderGraphPass& insert_earlydraw(RenderGraph& rg, SceneView* sceneView, GBufferPassHandles&& handles);
	RenderGraphPass& insert_latedraw(RenderGraph& rg, SceneView* sceneView, GBufferPassHandles&& handles);
};