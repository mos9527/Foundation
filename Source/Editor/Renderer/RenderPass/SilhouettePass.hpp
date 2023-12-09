#pragma once
#include "../Renderer.hpp"

struct SilhouettePass : public IRenderPass {
	std::unique_ptr<RHI::Shader> VS, PS,CS;	
	std::unique_ptr<RHI::PipelineState> PSO, PSO_Blend;
	std::unique_ptr<RHI::CommandSignature> IndirectCmdSig;

	std::unique_ptr<BufferContainer<SilhouetteConstants>> constants;
public:
	struct SilhouettePassHandles {
		std::pair<RgHandle*, RgHandle*> cmd_uav;
		std::tuple<RgHandle*, RgHandle*, RgHandle*> silhouette_dsv_srv;
		std::pair<RgHandle*, RgHandle*> frameBuffer_uav;		
	};

	SilhouettePass(RHI::Device* device) : IRenderPass(device, "Silouette Drawing") {};
	virtual void setup();
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, SilhouettePassHandles&& handles);
};