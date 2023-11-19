#pragma once
#include "RenderPass.hpp"

class GBufferPass : public RenderPass {
	virtual void insert_into(RenderGraph& rg) = 0;
};