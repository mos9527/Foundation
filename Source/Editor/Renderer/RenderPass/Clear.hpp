#pragma once
#include "RenderPass.hpp"

class ClearPass {
public:
	ClearPass(RHI::Device*) {};
	RenderGraphPass& insert_rtv(RenderGraph& rg, SceneView* sceneView, std::vector<RgHandle*>&& textures, std::vector<RgHandle*>&& textureRTVs);
	RenderGraphPass& insert_dsv(RenderGraph& rg, SceneView* sceneView, std::vector<RgHandle*>&& textures, std::vector<RgHandle*>&& textureDSVs);
};