#pragma once
#include "../../RHI/RHI.hpp"
#include "../../AssetRegistry/AssetRegistry.hpp"
#include "../../SceneGraph/SceneGraph.hpp"
#include "../../SceneGraph/SceneGraphView.hpp"
#include "../../RenderGraph/RenderGraph.hpp"
#include "../../RenderGraph/RenderGraphResource.hpp"
#include "../../RenderGraph/RenderGraphResourceCache.hpp"

class RenderPass {
public:
	virtual void insert_into(RenderGraph& rg) = 0;
};