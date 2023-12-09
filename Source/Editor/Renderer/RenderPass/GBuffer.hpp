#pragma once
#include "../Renderer.hpp"
struct GBufferPass : public IRenderPass {
private:
	std::unique_ptr<RHI::Shader> VS, PS;	
	std::unique_ptr<RHI::PipelineState> PSO, PSO_Wireframe;
	std::unique_ptr<RHI::CommandSignature> IndirectCmdSig;	
public:
	static const RHI::Resource::ResourceDesc GetTangentFrameDesc(uint width, uint height) {
		return RHI::Resource::ResourceDesc::GetTextureBufferDesc(
			ResourceFormat::R10G10B10A2_UNORM, ResourceDimension::Texture2D,
			width, height, 1, 1, 1, 0,
			ResourceFlags::RenderTarget, ResourceHeapType::Default,
			ResourceState::RenderTarget, ClearValue(0, 0, 0, 0),
			L"Tangent Frame"
		);
	}	
	static const RHI::Resource::ResourceDesc GetGradientDesc(uint width, uint height) {
		return RHI::Resource::ResourceDesc::GetTextureBufferDesc(
			ResourceFormat::R16G16B16A16_SNORM, ResourceDimension::Texture2D,
			width, height, 1, 1, 1, 0,
			ResourceFlags::RenderTarget, ResourceHeapType::Default,
			ResourceState::RenderTarget, ClearValue(0, 0, 0, 0),
			L"UV/Depth Gradient"
		);
	}
	static const RHI::Resource::ResourceDesc GetMaterialDesc(uint width, uint height) {
		return RHI::Resource::ResourceDesc::GetTextureBufferDesc(
			ResourceFormat::R16G16B16A16_UNORM, ResourceDimension::Texture2D,
			width, height, 1, 1, 1, 0,
			ResourceFlags::RenderTarget, ResourceHeapType::Default,
			ResourceState::RenderTarget, ClearValue(0, 0, 0, 0),
			L"Material ID/UV"
		);
	}
	struct Handles {
		std::pair<RgHandle*, RgHandle*> command_uav;
		std::pair<RgHandle*, RgHandle*> tangentframe_rtv;
		std::pair<RgHandle*, RgHandle*> gradient_rtv;
		std::pair<RgHandle*, RgHandle*> material_rtv;
		std::pair<RgHandle*, RgHandle*> depth_dsv;		
	};
	GBufferPass(RHI::Device* device) : IRenderPass(device, "GBuffer Generation") {};
	virtual void setup();
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles);	
};