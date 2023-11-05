#pragma once
#include "../RHI/RHI.hpp"
#include "../../IO/Image.hpp"


class TextureManager {
public:
	TextureManager(entt::registry& _registery) : registery(_registery) {};
	entt::entity LoadTexture(RHI::Device* device, RHI::CommandList* cmdList, IO::bitmap8bpp& bmp);

private:
	entt::registry& registery;
};
