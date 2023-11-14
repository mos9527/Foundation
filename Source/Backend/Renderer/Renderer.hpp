#pragma once
#include "../RHI/RHI.hpp"
#include "RenderGraph.hpp"
class Renderer {
protected:
	Device* device;
	entt::registry registry;
	RenderGraph rendergraph{ registry };
};