#pragma once
#include "../Renderer.hpp"
class DeferredLightingPass : public IRenderPass {
	std::unique_ptr<RHI::Shader> CS;
	std::unique_ptr<RHI::PipelineState> PSO;
	std::unique_ptr<BufferContainer<ShadingConstants>> constants;
public:
	struct Handles {
		std::pair<RgHandle*, RgHandle*> tangentframe_srv;
		std::pair<RgHandle*, RgHandle*> gradient_srv;
		std::pair<RgHandle*, RgHandle*> material_srv;
		std::pair<RgHandle*, RgHandle*> depth_srv;
		std::pair<RgHandle*, RgHandle*> framebuffer_uav;		
	};
	DeferredLightingPass(RHI::Device* device) : IRenderPass(device, "Deferred Shading") { reset(); };
	virtual void reset();

	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles);
};