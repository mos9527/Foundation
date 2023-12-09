#pragma once
#include "../Renderer.hpp"
struct ClearPass : public IRenderPass {
public:
	ClearPass(RHI::Device*) : IRenderPass(device, "Clear") {};
	RenderGraphPass& insert_rtv(RenderGraph& rg, SceneView* sceneView, std::vector<RgHandle*>&& textures, std::vector<RgHandle*>&& textureRTVs);
	RenderGraphPass& insert_dsv(RenderGraph& rg, SceneView* sceneView, std::vector<RgHandle*>&& textures, std::vector<RgHandle*>&& textureDSVs);
};
