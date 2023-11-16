#pragma once
#include "../pch.hpp"
#include "../Engine/RHI/RHI.hpp"
#include "../Engine/AssetRegistry/AssetRegistry.hpp"
#include "../Engine/SceneGraph/SceneGraph.hpp"
#include "../Engine/SceneGraph/SceneGraphView.hpp"
#include "../Engine/RenderGraph/RenderGraph.hpp"
#include "../Engine/RenderGraph/RenderGraphResourceCache.hpp"
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