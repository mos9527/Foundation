#pragma once
#include "../Renderer.hpp"
struct TransparencyPass : public IRenderPass {
	std::unique_ptr<RHI::Shader> VS, PS, materialPS, blendCS;
	std::unique_ptr<RHI::PipelineState> PSO, PSO_Blend, PSO_Material;
	std::unique_ptr<RHI::CommandSignature> IndirectCmdSig;
public:
	struct Handles {
		std::pair<RgHandle*, RgHandle*> cmd_uav;
		
		std::tuple<RgHandle*, RgHandle*, RgHandle*> accumaltion_rtv_srv;
		std::tuple<RgHandle*, RgHandle*, RgHandle*> revealage_rtv_srv;

		std::pair<RgHandle*, RgHandle*> depth_dsv;
		std::pair<RgHandle*, RgHandle*> fb_uav;
		std::pair<RgHandle*, RgHandle*> material_srv;		
	};

	TransparencyPass(RHI::Device* device) : IRenderPass(device) {};
	virtual void setup();
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles);
};