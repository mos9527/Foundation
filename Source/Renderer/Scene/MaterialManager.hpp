#pragma once
#include "Common.hpp"
#include "../RHI/RHI.hpp"
#include "../../IO/Image.hpp"
struct Material {
	std::string name;
	std::unique_ptr<RHI::Texture> texture_buffer;
};
class MaterialManager {
public:
	MaterialManager(entt::registry& _registery) : registery(_registery) {};
	entt::entity LoadTexture(RHI::Device* device, RHI::CommandList* cmdList, IO::bitmap8bpp& bmp);

private:
	entt::registry& registery;
};
