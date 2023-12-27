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
			RHI::ResourceFormat::R10G10B10A2_UNORM, RHI::ResourceDimension::Texture2D,
			width, height, 1, 1, 1, 0,
			RHI::ResourceFlags::RenderTarget, RHI::ResourceHeapType::Default,
			RHI::ResourceState::RenderTarget, RHI::ClearValue(0, 0, 0, 0),
			L"Tangent Frame"
		);
	}	
	static const RHI::Resource::ResourceDesc GetGradientDesc(uint width, uint height) {
		return RHI::Resource::ResourceDesc::GetTextureBufferDesc(
			RHI::ResourceFormat::R16G16B16A16_SNORM, RHI::ResourceDimension::Texture2D,
			width, height, 1, 1, 1, 0,
			RHI::ResourceFlags::RenderTarget, RHI::ResourceHeapType::Default,
			RHI::ResourceState::RenderTarget, RHI::ClearValue(0, 0, 0, 0),
			L"UV Gradient"
		);
	}
	static const RHI::Resource::ResourceDesc GetMaterialDesc(uint width, uint height) {
		return RHI::Resource::ResourceDesc::GetTextureBufferDesc(
			RHI::ResourceFormat::R16G16B16A16_UNORM, RHI::ResourceDimension::Texture2D,
			width, height, 1, 1, 1, 0,
			RHI::ResourceFlags::RenderTarget, RHI::ResourceHeapType::Default,
			RHI::ResourceState::RenderTarget, RHI::ClearValue(1, 1, 1, 1),
			L"Material ID/UV"
		);
	}
	static const RHI::Resource::ResourceDesc GetDepthDesc(uint width, uint height, const wchar_t* name = L"GBuffer Depth") {
		return RHI::Resource::ResourceDesc::GetTextureBufferDesc(
			RHI::ResourceFormat::D32_FLOAT, RHI::ResourceDimension::Texture2D,
			width, height, 1, 1, 1, 0,
			RHI::ResourceFlags::DepthStencil, RHI::ResourceHeapType::Default,
			RHI::ResourceState::DepthWrite,
#ifdef INVERSE_Z			
			RHI::ClearValue(0.0f, 0),
#else
			RHI::ClearValue(1.0f, 0),
#endif
			name
		);
	}
	struct Handles {
		std::pair<RgHandle*, RgHandle*> command_uav;
		std::pair<RgHandle*, RgHandle*> tangentframe_rtv;
		std::pair<RgHandle*, RgHandle*> gradient_rtv;
		std::pair<RgHandle*, RgHandle*> material_rtv;
		std::pair<RgHandle*, RgHandle*> depth_dsv;		
	};
	GBufferPass(RHI::Device* device) : IRenderPass(device, { L"GBuffer" }, "GBuffer Generation") { reset(); };
	virtual void reset();
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles);	
};