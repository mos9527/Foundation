#pragma once
#include "RenderPass.hpp"

class GBufferPass {
	std::unique_ptr<RHI::Shader> gBufferVS, gBufferPS;
	std::unique_ptr<RHI::RootSignature> gBufferRS;
	std::unique_ptr<RHI::PipelineState> gBufferPSO, gBufferPSOWireframe;
	std::unique_ptr<RHI::CommandSignature> gBufferIndirectCommandSig;
public:
	struct GBufferPassHandles {
		RgHandle& indirectCommands;
		RgHandle& indirectCommandsUAV;

		RgHandle& depth;
				
		RgHandle& albedo;
		RgHandle& normal;
		RgHandle& material;
		RgHandle& emissive;
				
		RgHandle& depth_dsv;
				
		RgHandle& albedo_rtv;
		RgHandle& normal_rtv;
		RgHandle& material_rtv;
		RgHandle& emissive_rtv;
	};
	GBufferPass(RHI::Device* device);
	void insert(RenderGraph& rg, SceneGraphView* sceneView, GBufferPassHandles& handles);
};