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
	static const RHI::Resource::ResourceDesc GetAccumalationDesc(uint width, uint height) {
		return RHI::Resource::ResourceDesc::GetTextureBufferDesc(
			ResourceFormat::R16G16B16A16_FLOAT, ResourceDimension::Texture2D,
			width, height, 1, 1, 1, 0,
			ResourceFlags::RenderTarget, ResourceHeapType::Default,
			ResourceState::RenderTarget, ClearValue(0, 0, 0, 0),
			L"Transparency Accumalation Buffer"
		);
	}
	static const RHI::Resource::ResourceDesc GetRevealgeDesc(uint width, uint height) {
		return RHI::Resource::ResourceDesc::GetTextureBufferDesc(
			ResourceFormat::R16_FLOAT, ResourceDimension::Texture2D,
			width, height, 1, 1, 1, 0,
			ResourceFlags::RenderTarget, ResourceHeapType::Default,
			ResourceState::RenderTarget, ClearValue(1, 1, 1, 1), // // !IMPORTANT. Revealage is dst * prod(1-a)
			L"Transparency Revealage Buffer"
		);
	}
	TransparencyPass(RHI::Device* device) : IRenderPass(device) { reset(); };
	virtual void reset();
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles);
};