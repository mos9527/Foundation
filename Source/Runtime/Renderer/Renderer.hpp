#pragma once
#include "../RHI/RHI.hpp"
#include "../AssetRegistry/AssetRegistry.hpp"
#include "../SceneGraph/SceneGraph.hpp"
#include "../SceneGraph/SceneGraphView.hpp"
#include "../RenderGraph/RenderGraph.hpp"
#include "../RenderGraph/RenderGraphResource.hpp"
#include "../RenderGraph/RenderGraphResourceCache.hpp"
class Renderer {
public:
	Renderer(AssetRegistry& assets, SceneGraph& scene, RHI::Device* device, RHI::Swapchain* swapchain) : assets(assets), scene(scene), device(device), swapchain(swapchain) {};
	virtual RHI::ShaderResourceView* Render() = 0;
protected:
	AssetRegistry& assets;
	SceneGraph& scene;	
	RHI::Device* device;
	RHI::Swapchain* swapchain;
};