#pragma once
#include "../RHI/RHI.hpp"

class Renderer {
protected:
	RHI::Device* device;
	entt::registry registry;
};