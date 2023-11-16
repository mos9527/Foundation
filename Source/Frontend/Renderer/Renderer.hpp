#pragma once
#include "../../pch.hpp"
#include "../../Backend/RHI/RHI.hpp"
#include "../../Backend/AssetRegistry/AssetRegistry.hpp"
#include "../../Backend/SceneGraph/SceneGraph.hpp"
#include "../../Backend/SceneGraph/SceneGraphView.hpp"
#include "../../Backend/RenderGraph/RenderGraph.hpp"
#include "../../Backend/RenderGraph/RenderGraphResourceCache.hpp"
class Renderer {
public:
	Renderer(AssetRegistry& assets, SceneGraph& scene, RHI::Device* device, RHI::Swapchain* swapchain) : assets(assets), scene(scene), device(device), swapchain(swapchain) {};
	virtual void Render() = 0;
protected:
	AssetRegistry& assets;
	SceneGraph& scene;	
	RHI::Device* device;
	RHI::Swapchain* swapchain;
};