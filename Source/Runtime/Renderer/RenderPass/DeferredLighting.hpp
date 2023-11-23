#pragma once
#include "RenderPass.hpp"

class DeferredLightingPass {
	std::unique_ptr<RHI::Shader> lightingCS;
	std::unique_ptr<RHI::RootSignature> lightingRS;
	std::unique_ptr<RHI::PipelineState> lightingPSO;
public:
	struct DeferredLightingPassHandles {
		RgHandle& frameBuffer;
				
		RgHandle& depth;
				
		RgHandle& albedo;
		RgHandle& normal;
		RgHandle& material;
		RgHandle& emissive;
				
		RgHandle& depth_srv;
		RgHandle& albedo_srv;
		RgHandle& normal_srv;
		RgHandle& material_srv;
		RgHandle& emissive_srv;

		RgHandle& fb_uav;		
	};
	DeferredLightingPass(RHI::Device* device);
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, DeferredLightingPassHandles&& handles);
};